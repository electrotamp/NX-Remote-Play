using NxRemotePlay.Client.Core;
using System;
using System.IO;
using System.Numerics;
using System.Runtime.InteropServices;
using System.Text;
using static SDL2.SDL;

namespace NxRemotePlay.Client.GUI
{
    // Picture upscaling on the graphics card (/29). The Switch sends 1280x720; when the video is shown larger than that the
    // normal path stretches it with plain bilinear filtering, which looks soft. With the OpenGL renderer this draws the video itself:
    //   FSR (default choice): AMD FidelityFX Super Resolution 1 (MIT, ThirdParty/fsr1): the decoded YUV picture is converted to RGB at its
    //     own size, EASU (edge-adaptive upscale) enlarges it to the size it is shown at, RCAS (robust contrast-adaptive sharpening) finishes it.
    //   Cubic: a lighter Catmull-Rom filter with an optional limited sharpening step (also the fallback when FSR cannot be set up).
    // Nothing is added to the stream, nothing changes on the Switch, and any problem (no OpenGL renderer, shader error, GL error) drops to
    // the next simpler path and finally to the normal stretch for the rest of the session. The FSR source is AMD's, the surrounding
    // passes are ours.
    internal static unsafe class GlUpscaler
    {
        const uint GL_VERTEX_SHADER = 0x8B31, GL_FRAGMENT_SHADER = 0x8B30, GL_COMPILE_STATUS = 0x8B81, GL_LINK_STATUS = 0x8B82, GL_CURRENT_PROGRAM = 0x8B8D;
        const uint GL_ALL_ATTRIB_BITS = 0xFFFFFFFF, GL_TRIANGLE_STRIP = 5, GL_BLEND = 0x0BE2, GL_SCISSOR_TEST = 0x0C11, GL_DEPTH_TEST = 0x0B71, GL_CULL_FACE = 0x0B44;
        const uint GL_TEXTURE_2D = 0x0DE1, GL_TEXTURE0 = 0x84C0, GL_FRAMEBUFFER = 0x8D40, GL_COLOR_ATTACHMENT0 = 0x8CE0, GL_FRAMEBUFFER_COMPLETE = 0x8CD5, GL_FRAMEBUFFER_BINDING = 0x8CA6;
        const uint GL_RGBA16F = 0x881A, GL_RGBA = 0x1908, GL_HALF_FLOAT = 0x140B, GL_NEAREST = 0x2600, GL_CLAMP_TO_EDGE = 0x812F;

        // ---- Cubic (Catmull-Rom) path: GLSL 1.20 ----
        const string CubicVertex = """
            #version 120
            varying vec2 uv;
            void main() { gl_Position = gl_Vertex; uv = gl_MultiTexCoord0.xy; }
            """;

        const string CubicFragment = """
            #version 120
            uniform sampler2D texY;
            uniform sampler2D texU;
            uniform sampler2D texV;
            uniform vec2 srcSize;
            uniform vec2 params;   // x: 1 = sharpen on, y: strength 0..1
            varying vec2 uv;

            float lumaCR(vec2 p) {
                vec2 pos = p * srcSize;
                vec2 c = floor(pos - 0.5) + 0.5;
                vec2 f = pos - c;
                vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
                vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
                vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
                vec2 w3 = f * f * (-0.5 + 0.5 * f);
                vec2 w12 = w1 + w2;
                vec2 t0 = (c - 1.0) / srcSize;
                vec2 t12 = (c + w2 / w12) / srcSize;
                vec2 t3 = (c + 2.0) / srcSize;
                float r = 0.0;
                r += texture2D(texY, vec2(t0.x, t0.y)).r * (w0.x * w0.y);
                r += texture2D(texY, vec2(t12.x, t0.y)).r * (w12.x * w0.y);
                r += texture2D(texY, vec2(t3.x, t0.y)).r * (w3.x * w0.y);
                r += texture2D(texY, vec2(t0.x, t12.y)).r * (w0.x * w12.y);
                r += texture2D(texY, vec2(t12.x, t12.y)).r * (w12.x * w12.y);
                r += texture2D(texY, vec2(t3.x, t12.y)).r * (w3.x * w12.y);
                r += texture2D(texY, vec2(t0.x, t3.y)).r * (w0.x * w3.y);
                r += texture2D(texY, vec2(t12.x, t3.y)).r * (w12.x * w3.y);
                r += texture2D(texY, vec2(t3.x, t3.y)).r * (w3.x * w3.y);
                return r;
            }

            void main() {
                float y = lumaCR(uv);
                if (params.x > 0.5) {
                    vec2 px = 1.0 / srcSize;
                    float c0 = texture2D(texY, uv).r;
                    float n = texture2D(texY, uv + vec2(0.0, -px.y)).r;
                    float s = texture2D(texY, uv + vec2(0.0, px.y)).r;
                    float e = texture2D(texY, uv + vec2(px.x, 0.0)).r;
                    float w = texture2D(texY, uv + vec2(-px.x, 0.0)).r;
                    float lo = min(c0, min(min(n, s), min(e, w)));
                    float hi = max(c0, max(max(n, s), max(e, w)));
                    float blur = (n + s + e + w + c0 * 2.0) / 6.0;
                    y = clamp(y + params.y * 1.5 * (y - blur), lo - 0.03, hi + 0.03);
                }
                float yy = (y - 16.0 / 255.0) * (255.0 / 219.0);
                float cb = (texture2D(texU, uv).r - 128.0 / 255.0) * (255.0 / 224.0);
                float cr = (texture2D(texV, uv).r - 128.0 / 255.0) * (255.0 / 224.0);
                vec3 rgb = vec3(yy + 1.5748 * cr, yy - 0.1873 * cb - 0.4681 * cr, yy + 1.8556 * cb);
                gl_FragColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);
            }
            """;

        // ---- FSR path: GLSL 4.00 (textureGather) ----
        const string FsrVertex = """
            #version 400 compatibility
            void main() { gl_Position = gl_Vertex; }
            """;

        // pass 1: the three video planes (BT.709, limited range, what the Switch's encoder writes) to RGB at the picture's own size
        const string YuvFragment = """
            #version 400 compatibility
            uniform sampler2D ty;
            uniform sampler2D tu;
            uniform sampler2D tv;
            uniform vec2 size;
            out vec4 o;
            void main() {
                vec2 q = gl_FragCoord.xy / size;
                float y = (texture(ty, q).r - 16.0 / 255.0) * (255.0 / 219.0);
                float u = (texture(tu, q).r - 128.0 / 255.0) * (255.0 / 224.0);
                float v = (texture(tv, q).r - 128.0 / 255.0) * (255.0 / 224.0);
                o = vec4(clamp(vec3(y + 1.5748 * v, y - 0.1873 * u - 0.4681 * v, y + 1.8556 * u), 0.0, 1.0), 1.0);
            }
            """;

        const string EasuHead = "#version 400 compatibility\n#extension GL_ARB_shading_language_packing : enable\n#define A_GPU 1\n#define A_GLSL 1\n";
        const string EasuMid = """
            #define FSR_EASU_F 1
            uniform sampler2D InputTexture;
            uniform uvec4 con0;
            uniform uvec4 con1;
            uniform uvec4 con2;
            uniform uvec4 con3;
            AF4 FsrEasuRF(AF2 p) { return textureGather(InputTexture, p, 0); }
            AF4 FsrEasuGF(AF2 p) { return textureGather(InputTexture, p, 1); }
            AF4 FsrEasuBF(AF2 p) { return textureGather(InputTexture, p, 2); }

            """;
        const string EasuTail = """

            out vec4 o;
            void main() {
                AF3 c;
                FsrEasuF(c, AU2(gl_FragCoord.xy), con0, con1, con2, con3);
                o = vec4(c, 1.0);
            }
            """;
        const string RcasMid = """
            #define FSR_RCAS_F 1
            uniform sampler2D InputTexture;
            uniform uvec4 con;
            uniform ivec4 vp;   // where the picture sits in the window: x, y (from the bottom), width, height
            AF4 FsrRcasLoadF(ASU2 p) { return texelFetch(InputTexture, clamp(ivec2(p), ivec2(0), vp.zw - 1), 0); }
            void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}

            """;
        const string RcasTail = """

            out vec4 o;
            void main() {
                ivec2 q = ivec2(gl_FragCoord.xy) - vp.xy;
                q.y = vp.w - 1 - q.y;   // the window counts rows from the bottom, the picture from the top
                AF1 r, g, b;
                FsrRcasF(r, g, b, AU2(q), con);
                o = vec4(r, g, b, 1.0);
            }
            """;

        static delegate* unmanaged<uint, uint> glCreateShader;
        static delegate* unmanaged<uint, int, byte**, int*, void> glShaderSource;
        static delegate* unmanaged<uint, void> glCompileShader;
        static delegate* unmanaged<uint, uint, int*, void> glGetShaderiv;
        static delegate* unmanaged<uint, int, int*, byte*, void> glGetShaderInfoLog;
        static delegate* unmanaged<uint> glCreateProgram;
        static delegate* unmanaged<uint, uint, void> glAttachShader;
        static delegate* unmanaged<uint, void> glLinkProgram;
        static delegate* unmanaged<uint, uint, int*, void> glGetProgramiv;
        static delegate* unmanaged<uint, int, int*, byte*, void> glGetProgramInfoLog;
        static delegate* unmanaged<uint, void> glUseProgram;
        static delegate* unmanaged<uint, byte*, int> glGetUniformLocation;
        static delegate* unmanaged<int, int, void> glUniform1i;
        static delegate* unmanaged<int, float, float, void> glUniform2f;
        static delegate* unmanaged<int, uint, uint, uint, uint, void> glUniform4ui;
        static delegate* unmanaged<int, int, int, int, int, void> glUniform4i;
        static delegate* unmanaged<uint, void> glPushAttrib;
        static delegate* unmanaged<void> glPopAttrib;
        static delegate* unmanaged<int, int, int, int, void> glViewport;
        static delegate* unmanaged<uint, void> glBegin;
        static delegate* unmanaged<void> glEnd;
        static delegate* unmanaged<float, float, void> glVertex2f;
        static delegate* unmanaged<float, float, void> glTexCoord2f;
        static delegate* unmanaged<uint, int*, void> glGetIntegerv;
        static delegate* unmanaged<uint> glGetError;
        static delegate* unmanaged<uint, void> glDisable;
        static delegate* unmanaged<int, uint*, void> glGenFramebuffers;
        static delegate* unmanaged<uint, uint, void> glBindFramebuffer;
        static delegate* unmanaged<uint, uint, uint, uint, int, void> glFramebufferTexture2D;
        static delegate* unmanaged<uint, uint> glCheckFramebufferStatus;
        static delegate* unmanaged<int, uint*, void> glGenTextures;
        static delegate* unmanaged<int, uint*, void> glDeleteTextures;
        static delegate* unmanaged<uint, uint, void> glBindTexture;
        static delegate* unmanaged<uint, int, int, int, int, int, uint, uint, void*, void> glTexImage2D;
        static delegate* unmanaged<uint, uint, int, void> glTexParameteri;
        static delegate* unmanaged<uint, void> glActiveTexture;

        static bool initTried, ready, failed, fsrReady, fsrFailed;
        static uint cubicProgram;
        static int locY, locU, locV, locSrc, locParams, errors;
        static uint yuvProgram, easuProgram, rcasProgram, fbo, texRgb, texEasu;
        static int rgbW, rgbH, easuW, easuH;
        static int yuvTy, yuvTu, yuvTv, yuvSize, easuInput, easuC0, easuC1, easuC2, easuC3, rcasInput, rcasCon, rcasVp;

        // Set before the renderer is created: the normal (Direct3D) renderer has no shaders, the OpenGL one does.
        public static bool WantsOpenGlRenderer => Program.Options.Upscale != UpscaleMode.Off && !Program.Options.ForceSoftwareRenderer;
        public static bool Working => ready && !failed;
        public static string Status => failed ? "off (a problem was logged)" : !ready ? "waiting for the first frame"
            : Program.Options.Upscale == UpscaleMode.Fsr ? (fsrReady && !fsrFailed ? "FSR 1 (EASU + RCAS)" : "FSR unavailable, cubic filter in use") : "cubic filter";

        static void Fail(string why)
        {
            failed = true;
            Program.LogError("upscaler", why + " - upscaling is off for this session, the normal picture path is used");
            Console.WriteLine("Upscaler off: " + why);
        }

        static nint Proc(string name)
        {
            var p = SDL_GL_GetProcAddress(name);
            if (p == IntPtr.Zero) throw new Exception("OpenGL function " + name + " is not available");
            return p;
        }

        static string Log(uint obj, bool isShader)
        {
            int len = 0;
            if (isShader) glGetShaderiv(obj, 0x8B84, &len); else glGetProgramiv(obj, 0x8B84, &len);
            if (len <= 1) return "";
            var buf = new byte[len];
            int got = 0;
            fixed (byte* b = buf) { if (isShader) glGetShaderInfoLog(obj, len, &got, b); else glGetProgramInfoLog(obj, len, &got, b); }
            return Encoding.ASCII.GetString(buf, 0, Math.Max(0, Math.Min(got, len)));
        }

        static uint Compile(uint type, string src, string what)
        {
            uint sh = glCreateShader(type);
            var bytes = Encoding.ASCII.GetBytes(src + "\0");
            fixed (byte* p = bytes)
            {
                byte* pp = p;
                int len = bytes.Length - 1;
                glShaderSource(sh, 1, &pp, &len);
            }
            glCompileShader(sh);
            int ok = 0;
            glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
            if (ok == 0) throw new Exception(what + " shader: " + Log(sh, true));
            return sh;
        }

        static uint Link(string vertex, string fragment, string what)
        {
            uint vs = Compile(GL_VERTEX_SHADER, vertex, what + " vertex"), fs = Compile(GL_FRAGMENT_SHADER, fragment, what + " fragment");
            uint program = glCreateProgram();
            glAttachShader(program, vs);
            glAttachShader(program, fs);
            glLinkProgram(program);
            int ok = 0;
            glGetProgramiv(program, GL_LINK_STATUS, &ok);
            if (ok == 0) throw new Exception(what + " link: " + Log(program, false));
            return program;
        }

        static int Loc(uint program, string name)
        {
            var b = Encoding.ASCII.GetBytes(name + "\0");
            fixed (byte* p = b) return glGetUniformLocation(program, p);
        }

        static string Resource(string name)
        {
            using var s = typeof(GlUpscaler).Assembly.GetManifestResourceStream(name) ?? throw new Exception("missing resource " + name);
            using var r = new StreamReader(s, Encoding.ASCII);
            return r.ReadToEnd();
        }

        static void LoadProcs()
        {
            glCreateShader = (delegate* unmanaged<uint, uint>)Proc("glCreateShader");
            glShaderSource = (delegate* unmanaged<uint, int, byte**, int*, void>)Proc("glShaderSource");
            glCompileShader = (delegate* unmanaged<uint, void>)Proc("glCompileShader");
            glGetShaderiv = (delegate* unmanaged<uint, uint, int*, void>)Proc("glGetShaderiv");
            glGetShaderInfoLog = (delegate* unmanaged<uint, int, int*, byte*, void>)Proc("glGetShaderInfoLog");
            glCreateProgram = (delegate* unmanaged<uint>)Proc("glCreateProgram");
            glAttachShader = (delegate* unmanaged<uint, uint, void>)Proc("glAttachShader");
            glLinkProgram = (delegate* unmanaged<uint, void>)Proc("glLinkProgram");
            glGetProgramiv = (delegate* unmanaged<uint, uint, int*, void>)Proc("glGetProgramiv");
            glGetProgramInfoLog = (delegate* unmanaged<uint, int, int*, byte*, void>)Proc("glGetProgramInfoLog");
            glUseProgram = (delegate* unmanaged<uint, void>)Proc("glUseProgram");
            glGetUniformLocation = (delegate* unmanaged<uint, byte*, int>)Proc("glGetUniformLocation");
            glUniform1i = (delegate* unmanaged<int, int, void>)Proc("glUniform1i");
            glUniform2f = (delegate* unmanaged<int, float, float, void>)Proc("glUniform2f");
            glPushAttrib = (delegate* unmanaged<uint, void>)Proc("glPushAttrib");
            glPopAttrib = (delegate* unmanaged<void>)Proc("glPopAttrib");
            glViewport = (delegate* unmanaged<int, int, int, int, void>)Proc("glViewport");
            glBegin = (delegate* unmanaged<uint, void>)Proc("glBegin");
            glEnd = (delegate* unmanaged<void>)Proc("glEnd");
            glVertex2f = (delegate* unmanaged<float, float, void>)Proc("glVertex2f");
            glTexCoord2f = (delegate* unmanaged<float, float, void>)Proc("glTexCoord2f");
            glGetIntegerv = (delegate* unmanaged<uint, int*, void>)Proc("glGetIntegerv");
            glGetError = (delegate* unmanaged<uint>)Proc("glGetError");
            glDisable = (delegate* unmanaged<uint, void>)Proc("glDisable");
            glGenTextures = (delegate* unmanaged<int, uint*, void>)Proc("glGenTextures");
            glDeleteTextures = (delegate* unmanaged<int, uint*, void>)Proc("glDeleteTextures");
            glBindTexture = (delegate* unmanaged<uint, uint, void>)Proc("glBindTexture");
            glTexImage2D = (delegate* unmanaged<uint, int, int, int, int, int, uint, uint, void*, void>)Proc("glTexImage2D");
            glTexParameteri = (delegate* unmanaged<uint, uint, int, void>)Proc("glTexParameteri");
            glActiveTexture = (delegate* unmanaged<uint, void>)Proc("glActiveTexture");
        }

        // Needs the renderer's OpenGL context to be current (SDL_GL_BindTexture makes it so).
        static void InitCubic()
        {
            cubicProgram = Link(CubicVertex, CubicFragment, "cubic");
            locY = Loc(cubicProgram, "texY"); locU = Loc(cubicProgram, "texU"); locV = Loc(cubicProgram, "texV"); locSrc = Loc(cubicProgram, "srcSize"); locParams = Loc(cubicProgram, "params");
            if (locY < 0 || locU < 0 || locV < 0 || locSrc < 0 || locParams < 0) throw new Exception("a cubic shader input was optimised away");
        }

        static void InitFsr()
        {
            glUniform4ui = (delegate* unmanaged<int, uint, uint, uint, uint, void>)Proc("glUniform4ui");
            glUniform4i = (delegate* unmanaged<int, int, int, int, int, void>)Proc("glUniform4i");
            glGenFramebuffers = (delegate* unmanaged<int, uint*, void>)Proc("glGenFramebuffers");
            glBindFramebuffer = (delegate* unmanaged<uint, uint, void>)Proc("glBindFramebuffer");
            glFramebufferTexture2D = (delegate* unmanaged<uint, uint, uint, uint, int, void>)Proc("glFramebufferTexture2D");
            glCheckFramebufferStatus = (delegate* unmanaged<uint, uint>)Proc("glCheckFramebufferStatus");
            string ffxA = Resource("fsr1.ffx_a.glsl"), ffxFsr = Resource("fsr1.ffx_fsr1.glsl");
            yuvProgram = Link(FsrVertex, YuvFragment, "FSR YUV");
            easuProgram = Link(FsrVertex, EasuHead + ffxA + EasuMid + ffxFsr + EasuTail, "FSR EASU");
            rcasProgram = Link(FsrVertex, EasuHead + ffxA + RcasMid + ffxFsr + RcasTail, "FSR RCAS");
            yuvTy = Loc(yuvProgram, "ty"); yuvTu = Loc(yuvProgram, "tu"); yuvTv = Loc(yuvProgram, "tv"); yuvSize = Loc(yuvProgram, "size");
            easuInput = Loc(easuProgram, "InputTexture"); easuC0 = Loc(easuProgram, "con0"); easuC1 = Loc(easuProgram, "con1"); easuC2 = Loc(easuProgram, "con2"); easuC3 = Loc(easuProgram, "con3");
            rcasInput = Loc(rcasProgram, "InputTexture"); rcasCon = Loc(rcasProgram, "con"); rcasVp = Loc(rcasProgram, "vp");
            if (yuvTy < 0 || yuvTu < 0 || yuvTv < 0 || yuvSize < 0 || easuInput < 0 || easuC0 < 0 || easuC3 < 0 || rcasInput < 0 || rcasCon < 0 || rcasVp < 0)
                throw new Exception("an FSR shader input was optimised away");
            uint f = 0;
            glGenFramebuffers(1, &f);
            fbo = f;
        }

        static uint Bits(float v) => BitConverter.SingleToUInt32Bits(v);

        // AMD's FsrEasuCon / FsrRcasCon (the constant set-up that ffx_fsr1.h provides for the CPU), ported to C#
        static void EasuConstants(float inW, float inH, float outW, float outH, out uint[] c0, out uint[] c1, out uint[] c2, out uint[] c3)
        {
            c0 = new[] { Bits(inW / outW), Bits(inH / outH), Bits(0.5f * inW / outW - 0.5f), Bits(0.5f * inH / outH - 0.5f) };
            c1 = new[] { Bits(1f / inW), Bits(1f / inH), Bits(1f / inW), Bits(-1f / inH) };
            c2 = new[] { Bits(-1f / inW), Bits(2f / inH), Bits(1f / inW), Bits(2f / inH) };
            c3 = new[] { Bits(0f / inW), Bits(4f / inH), 0u, 0u };
        }

        static uint[] RcasConstants(float stops)
        {
            float sharp = MathF.Pow(2f, -stops);
            uint half = BitConverter.HalfToUInt16Bits((Half)sharp);
            return new[] { Bits(sharp), half | (half << 16), 0u, 0u };
        }

        static void MakeTexture(ref uint tex, ref int curW, ref int curH, int w, int h)
        {
            if (tex != 0 && curW == w && curH == h) return;
            if (tex != 0) { uint old = tex; glDeleteTextures(1, &old); }
            uint t = 0;
            glGenTextures(1, &t);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, t);
            glTexImage2D(GL_TEXTURE_2D, 0, (int)GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, null);
            glTexParameteri(GL_TEXTURE_2D, 0x2801, (int)GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, 0x2800, (int)GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, 0x2802, (int)GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, 0x2803, (int)GL_CLAMP_TO_EDGE);
            tex = t; curW = w; curH = h;
        }

        static void Attach(uint tex)
        {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) throw new Exception("framebuffer incomplete");
        }

        static void Quad(float x0, float y0, float x1, float y1)
        {
            glBegin(GL_TRIANGLE_STRIP);
            glTexCoord2f(0, 0); glVertex2f(x0, y0);
            glTexCoord2f(1, 0); glVertex2f(x1, y0);
            glTexCoord2f(0, 1); glVertex2f(x0, y1);
            glTexCoord2f(1, 1); glVertex2f(x1, y1);
            glEnd();
        }

        // FSR: planes -> RGB (source size) -> EASU (shown size) -> RCAS (to the window). Runs inside the caller's PushAttrib/PopAttrib,
        // with the video planes bound to texture units 0..2 by SDL_GL_BindTexture. The caller restores the framebuffer binding.
        static void DrawFsr(nint texture, int srcW, int srcH, int px, int py, int dw, int dh, int pw, int ph, int prevFbo)
        {
            MakeTexture(ref texRgb, ref rgbW, ref rgbH, srcW, srcH);
            MakeTexture(ref texEasu, ref easuW, ref easuH, dw, dh);
            SDL_GL_BindTexture(texture, out _, out _);   // MakeTexture used unit 0: put the video planes back
            // pass 1: YUV -> RGB
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            Attach(texRgb);
            glViewport(0, 0, srcW, srcH);
            glUseProgram(yuvProgram);
            glUniform1i(yuvTy, 0); glUniform1i(yuvTu, 1); glUniform1i(yuvTv, 2);
            glUniform2f(yuvSize, srcW, srcH);
            Quad(-1, -1, 1, 1);
            // pass 2: EASU
            Attach(texEasu);
            glViewport(0, 0, dw, dh);
            glUseProgram(easuProgram);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, texRgb);
            glUniform1i(easuInput, 0);
            EasuConstants(srcW, srcH, dw, dh, out var c0, out var c1, out var c2, out var c3);
            glUniform4ui(easuC0, c0[0], c0[1], c0[2], c0[3]); glUniform4ui(easuC1, c1[0], c1[1], c1[2], c1[3]);
            glUniform4ui(easuC2, c2[0], c2[1], c2[2], c2[3]); glUniform4ui(easuC3, c3[0], c3[1], c3[2], c3[3]);
            Quad(-1, -1, 1, 1);
            // pass 3: RCAS to the window
            glBindFramebuffer(GL_FRAMEBUFFER, (uint)prevFbo);
            glViewport(0, 0, pw, ph);
            glUseProgram(rcasProgram);
            glBindTexture(GL_TEXTURE_2D, texEasu);
            glUniform1i(rcasInput, 0);
            // 0 stops = sharpest, 2 = none; strength 100 -> 0, 75 -> 0.25 (AMD's own default is about 0.2), 0 -> 1
            float stops = (100 - Math.Clamp(Program.Options.UpscaleStrength, 0, 100)) / 100f;
            var rc = RcasConstants(stops);
            glUniform4ui(rcasCon, rc[0], rc[1], rc[2], rc[3]);
            glUniform4i(rcasVp, px, ph - (py + dh), dw, dh);
            Quad(px / (float)pw * 2f - 1f, 1f - py / (float)ph * 2f, (px + dw) / (float)pw * 2f - 1f, 1f - (py + dh) / (float)ph * 2f);
        }

        static void DrawCubic(int srcW, int srcH, float x0, float y0, float x1, float y1, int pw, int ph)
        {
            glViewport(0, 0, pw, ph);
            glUseProgram(cubicProgram);
            glUniform1i(locY, 0); glUniform1i(locU, 1); glUniform1i(locV, 2);
            glUniform2f(locSrc, srcW, srcH);
            int strength = Math.Clamp(Program.Options.UpscaleStrength, 0, 100);
            glUniform2f(locParams, strength > 0 ? 1f : 0f, strength / 100f);
            Quad(x0, y0, x1, y1);
        }

        // Draws the video planes of `texture` (an IYUV SDL texture of srcW x srcH) into `dst` (window units; dpi = pixels per unit).
        // False = nothing was drawn, the caller uses the normal path (also for the cases where upscaling does not apply).
        public static bool TryDraw(nint renderer, nint texture, SDL_Rect dst, Vector2 dpi, int srcW, int srcH)
        {
            var mode = Program.Options.Upscale;
            if (mode == UpscaleMode.Off || failed) return false;
            float sx = dpi.X > 0 ? dpi.X : 1, sy = dpi.Y > 0 ? dpi.Y : 1;
            int px = (int)MathF.Round(dst.x * sx), py = (int)MathF.Round(dst.y * sy), dw = (int)MathF.Round(dst.w * sx), dh = (int)MathF.Round(dst.h * sy);
            if (dw < srcW * 1.05f || dh < srcH * 1.05f) return false;   // not enlarging: nothing to improve
            try
            {
                SDL_GetRendererInfo(renderer, out var info);
                if (Marshal.PtrToStringAnsi(info.name) != "opengl") { if (!initTried) { initTried = true; Fail("the renderer is " + Marshal.PtrToStringAnsi(info.name) + ", not OpenGL"); } return false; }
                SDL_RenderFlush(renderer);
                if (!ready)
                {
                    initTried = true;
                    if (SDL_GL_BindTexture(texture, out _, out _) != 0) { Fail("SDL_GL_BindTexture: " + SDL_GetError()); return false; }
                    try
                    {
                        LoadProcs();
                        InitCubic();
                        ready = true;
                        try { InitFsr(); fsrReady = true; Console.WriteLine("Upscaler on (OpenGL, FSR 1 and cubic filters ready, mode " + mode + ")"); }
                        catch (Exception ex) { fsrFailed = true; Program.LogError("upscaler", "FSR could not be set up, the cubic filter is used instead: " + ex.Message); Console.WriteLine("FSR unavailable: " + ex.Message); }
                        while (glGetError() != 0) { }
                    }
                    finally { SDL_GL_UnbindTexture(texture); }
                }
                SDL_GetRendererOutputSize(renderer, out int pw, out int ph);
                glPushAttrib(GL_ALL_ATTRIB_BITS);
                int prevProgram = 0, prevFbo = 0;
                glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
                if (fsrReady) glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
                if (SDL_GL_BindTexture(texture, out _, out _) != 0) { glPopAttrib(); Fail("SDL_GL_BindTexture: " + SDL_GetError()); return false; }
                glDisable(GL_BLEND); glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
                bool useFsr = mode == UpscaleMode.Fsr && fsrReady && !fsrFailed;
                string problem = "";
                try
                {
                    if (useFsr) DrawFsr(texture, srcW, srcH, px, py, dw, dh, pw, ph, prevFbo);
                    else DrawCubic(srcW, srcH, px / (float)pw * 2f - 1f, 1f - py / (float)ph * 2f, (px + dw) / (float)pw * 2f - 1f, 1f - (py + dh) / (float)ph * 2f, pw, ph);
                }
                catch (Exception ex) { problem = ex.Message; }
                if (fsrReady) glBindFramebuffer(GL_FRAMEBUFFER, (uint)prevFbo);
                glUseProgram((uint)prevProgram);
                SDL_GL_UnbindTexture(texture);
                uint err = glGetError();
                glPopAttrib();
                if (problem.Length > 0 || err != 0)
                {
                    if (useFsr) { fsrFailed = true; Program.LogError("upscaler", "FSR stopped (" + (problem.Length > 0 ? problem : "OpenGL error 0x" + err.ToString("X")) + "), the cubic filter is used instead"); }
                    else if (++errors > 3) Fail(problem.Length > 0 ? problem : "OpenGL error 0x" + err.ToString("X"));
                    return false;
                }
                return true;
            }
            catch (Exception ex)
            {
                Fail(ex.Message);
                return false;
            }
        }
    }
}
