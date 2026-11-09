package com.nxremoteplay.punchclient

import android.content.res.AssetManager
import android.graphics.SurfaceTexture
import android.opengl.EGL14
import android.opengl.EGLConfig
import android.opengl.EGLContext
import android.opengl.EGLDisplay
import android.opengl.EGLSurface
import android.opengl.GLES11Ext
import android.opengl.GLES20
import android.opengl.GLES30
import android.os.Handler
import android.os.HandlerThread
import android.view.Surface
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

// Picture upscaling . Without this the H.264 decoder renders straight into the player's
// SurfaceView and Android stretches the
// 1280x720 picture with its own filter when the screen is bigger. With it the decoder renders into
// a SurfaceTexture instead and this class
// draws each frame onto the SurfaceView:
//   mode 1 = FSR: AMD FidelityFX Super Resolution 1 (MIT, assets/fsr): the frame is copied to an
// RGB texture, EASU (edge-adaptive upscale)
//            enlarges it to the screen size, RCAS (contrast-adaptive sharpening) finishes it. Needs
// OpenGL ES 3.0 (otherwise Cubic is used).
//   mode 2 = Cubic: a lighter Catmull-Rom filter with a little sharpening (OpenGL ES 2.0).
// It only enlarges when the surface is at least 1.05x the video; otherwise it draws plain. Nothing
// changes on the Switch or in the stream.
// Any setup failure throws so the caller keeps the normal surface; an FSR failure falls back to
// Cubic; a drawing failure stops the
// drawing and logs. All rows below are kept bottom-up (OpenGL's own order) so no flip is needed.
class UpscaleRenderer(
    private val outSurface: Surface,
    private val mode: Int,
    private val assets: AssetManager?,
    private val onLog: (String) -> Unit
) {
    private val thread = HandlerThread("NX-Upscale").apply { start() }
    private val handler = Handler(thread.looper)

    lateinit var inputSurface: Surface
        private set

    @Volatile private var released = false
    private var broken = false
    private var display: EGLDisplay = EGL14.EGL_NO_DISPLAY
    private var context: EGLContext = EGL14.EGL_NO_CONTEXT
    private var eglSurface: EGLSurface = EGL14.EGL_NO_SURFACE
    private var surfaceTexture: SurfaceTexture? = null
    private var program = 0 // OES program: plain / cubic / sharpen
    private var texId = 0
    private var uTex = 0
    private var uSrc = 0
    private var uParams = 0
    private var uTexture = 0
    private val matrix = FloatArray(16)
    private val size = IntArray(1)
    private var frames = 0L
    private var glErrors = 0
    private var glesVersion = 2
    // FSR
    private var fsrOk = false
    private var easuProgram = 0
    private var rcasProgram = 0
    private var easuInput = 0
    private var easuCon = IntArray(4)
    private var rcasInput = 0
    private var rcasCon = 0
    private var rcasSz = 0
    private var fbo = 0
    private var rgbTex = 0
    private var rgbW = 0
    private var rgbH = 0
    private var outTex = 0
    private var outW = 0
    private var outH = 0

    // the stream is 1280x720 on every current host
    @Volatile var sourceWidth = 1280
    @Volatile var sourceHeight = 720
    @Volatile var strength = 0.5f

    init {
        val latch = CountDownLatch(1)
        var failure: Throwable? = null
        handler.post {
            try {
                setup()
            } catch (t: Throwable) {
                failure = t
            } finally {
                latch.countDown()
            }
        }
        val finished = latch.await(5, TimeUnit.SECONDS)
        if (!finished || failure != null || !::inputSurface.isInitialized) {
            release()
            throw IllegalStateException(
                failure?.let { "${it.javaClass.simpleName}: ${it.message}" }
                    ?: "setup did not finish")
        }
    }

    private fun check(what: String) {
        val e = GLES20.glGetError()
        if (e != 0) throw IllegalStateException("$what: GL error 0x${Integer.toHexString(e)}")
    }

    private fun compile(type: Int, src: String, what: String): Int {
        val s = GLES20.glCreateShader(type)
        GLES20.glShaderSource(s, src)
        GLES20.glCompileShader(s)
        val ok = IntArray(1)
        GLES20.glGetShaderiv(s, GLES20.GL_COMPILE_STATUS, ok, 0)
        if (ok[0] == 0) throw IllegalStateException("$what shader: " + GLES20.glGetShaderInfoLog(s))
        return s
    }

    private fun link(vertex: String, fragment: String, what: String): Int {
        val p = GLES20.glCreateProgram()
        GLES20.glAttachShader(p, compile(GLES20.GL_VERTEX_SHADER, vertex, "$what vertex"))
        GLES20.glAttachShader(p, compile(GLES20.GL_FRAGMENT_SHADER, fragment, "$what fragment"))
        GLES20.glBindAttribLocation(
            p,
            0,
            "aPos") // ES 3.00 shaders pin it with layout(location=0); the ES 1.00 one needs this
        GLES20.glLinkProgram(p)
        val ok = IntArray(1)
        GLES20.glGetProgramiv(p, GLES20.GL_LINK_STATUS, ok, 0)
        if (ok[0] == 0) throw IllegalStateException("$what link: " + GLES20.glGetProgramInfoLog(p))
        return p
    }

    private fun createContext(config: EGLConfig?, version: Int): EGLContext =
        EGL14.eglCreateContext(
            display,
            config,
            EGL14.EGL_NO_CONTEXT,
            intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, version, EGL14.EGL_NONE),
            0)

    private fun setup() {
        display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
        if (display == EGL14.EGL_NO_DISPLAY) throw IllegalStateException("no EGL display")
        val version = IntArray(2)
        if (!EGL14.eglInitialize(display, version, 0, version, 1))
            throw IllegalStateException("eglInitialize")
        val attribs =
            intArrayOf(
                EGL14.EGL_RENDERABLE_TYPE,
                EGL14.EGL_OPENGL_ES2_BIT,
                EGL14.EGL_SURFACE_TYPE,
                EGL14.EGL_WINDOW_BIT,
                EGL14.EGL_RED_SIZE,
                8,
                EGL14.EGL_GREEN_SIZE,
                8,
                EGL14.EGL_BLUE_SIZE,
                8,
                EGL14.EGL_ALPHA_SIZE,
                0,
                EGL14.EGL_NONE,
            )
        val configs = arrayOfNulls<EGLConfig>(1)
        val count = IntArray(1)
        if (!EGL14.eglChooseConfig(display, attribs, 0, configs, 0, 1, count, 0) || count[0] < 1)
            throw IllegalStateException("no EGL config")
        // OpenGL ES 3.0 first (FSR needs it), plain 2.0 otherwise (Cubic only)
        context = if (mode == 1) createContext(configs[0], 3) else EGL14.EGL_NO_CONTEXT
        glesVersion = 3
        if (context == EGL14.EGL_NO_CONTEXT) {
            context = createContext(configs[0], 2)
            glesVersion = 2
        }
        if (context == EGL14.EGL_NO_CONTEXT) throw IllegalStateException("no GLES context")
        eglSurface =
            EGL14.eglCreateWindowSurface(
                display, configs[0], outSurface, intArrayOf(EGL14.EGL_NONE), 0)
        if (eglSurface == EGL14.EGL_NO_SURFACE) throw IllegalStateException("no window surface")
        if (!EGL14.eglMakeCurrent(display, eglSurface, eglSurface, context))
            throw IllegalStateException("eglMakeCurrent")
        EGL14.eglSwapInterval(display, 0) // show each frame as soon as it is drawn

        program = link(VERTEX, FRAGMENT, "OES")
        GLES20.glUseProgram(program)
        uTex = GLES20.glGetUniformLocation(program, "uTex")
        uSrc = GLES20.glGetUniformLocation(program, "uSrc")
        uParams = GLES20.glGetUniformLocation(program, "uParams")
        uTexture = GLES20.glGetUniformLocation(program, "uTexture")
        val quad = ByteBuffer.allocateDirect(8 * 4).order(ByteOrder.nativeOrder()).asFloatBuffer()
        quad.put(floatArrayOf(-1f, -1f, 1f, -1f, -1f, 1f, 1f, 1f)).position(0)
        GLES20.glEnableVertexAttribArray(0) // the quad is attribute 0 in every program (see link())
        GLES20.glVertexAttribPointer(0, 2, GLES20.GL_FLOAT, false, 0, quad)
        val tex = IntArray(1)
        GLES20.glGenTextures(1, tex, 0)
        texId = tex[0]
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, texId)
        GLES20.glTexParameteri(
            GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(
            GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(
            GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(
            GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)
        check("setup")
        if (mode == 1 && glesVersion >= 3) {
            try {
                setupFsr()
                fsrOk = true
            } catch (t: Throwable) {
                onLog(
                    "Upscale: FSR could not be set up (${t.javaClass.simpleName}: ${t.message}) - the cubic filter is used")
                while (GLES20.glGetError() != 0) {}
            }
        } else if (mode == 1)
            onLog(
                "Upscale: OpenGL ES 3.0 is not available - the cubic filter is used instead of FSR")
        val st = SurfaceTexture(texId)
        st.setDefaultBufferSize(sourceWidth, sourceHeight)
        st.setOnFrameAvailableListener({ draw() }, handler)
        surfaceTexture = st
        inputSurface = Surface(st)
        onLog(
            "Upscale: OpenGL ES ${glesVersion}.x ready (${if (mode == 1 && fsrOk) "FSR 1" else "cubic filter"})")
    }

    private fun readAsset(name: String): String {
        val am = assets ?: throw IllegalStateException("no asset manager")
        return am.open(name).use { String(it.readBytes(), Charsets.US_ASCII) }
    }

    private fun setupFsr() {
        val ffxA = readAsset("fsr/ffx_a_es.glsl")
        val ffxFsr = readAsset("fsr/ffx_fsr1.glsl")
        easuProgram = link(FSR_VERTEX, FSR_HEAD + ffxA + EASU_MID + ffxFsr + EASU_TAIL, "FSR EASU")
        rcasProgram = link(FSR_VERTEX, FSR_HEAD + ffxA + RCAS_MID + ffxFsr + RCAS_TAIL, "FSR RCAS")
        easuInput = GLES20.glGetUniformLocation(easuProgram, "InputTexture")
        for (i in 0..3) easuCon[i] = GLES20.glGetUniformLocation(easuProgram, "con$i")
        rcasInput = GLES20.glGetUniformLocation(rcasProgram, "InputTexture")
        rcasCon = GLES20.glGetUniformLocation(rcasProgram, "con")
        rcasSz = GLES20.glGetUniformLocation(rcasProgram, "sz")
        if (easuInput < 0 || easuCon.any { it < 0 } || rcasInput < 0 || rcasCon < 0 || rcasSz < 0)
            throw IllegalStateException("an FSR shader input was optimised away")
        val f = IntArray(1)
        GLES30.glGenFramebuffers(1, f, 0)
        fbo = f[0]
        check("FSR setup")
    }

    private fun bits(v: Float): Int = java.lang.Float.floatToRawIntBits(v)

    // float32 -> float16 bits (round to nearest), for AMD's packed RCAS sharpness constant
    private fun half(v: Float): Int {
        val x = bits(v)
        val sign = (x ushr 16) and 0x8000
        var e = ((x ushr 23) and 0xff) - 127 + 15
        var m = x and 0x7fffff
        if (e <= 0) {
            if (e < -10) return sign
            m = (m or 0x800000) shr (1 - e)
            return sign or ((m + 0x1000) shr 13)
        }
        if (e >= 31) return sign or 0x7c00
        return sign or (e shl 10) or ((m + 0x1000) shr 13)
    }

    private fun makeTexture(old: Int, w: Int, h: Int): Int {
        if (old != 0) GLES20.glDeleteTextures(1, intArrayOf(old), 0)
        val t = IntArray(1)
        GLES20.glGenTextures(1, t, 0)
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, t[0])
        GLES20.glTexImage2D(
            GLES20.GL_TEXTURE_2D,
            0,
            GLES20.GL_RGBA,
            w,
            h,
            0,
            GLES20.GL_RGBA,
            GLES20.GL_UNSIGNED_BYTE,
            null)
        GLES20.glTexParameteri(
            GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_NEAREST)
        GLES20.glTexParameteri(
            GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_NEAREST)
        GLES20.glTexParameteri(
            GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(
            GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)
        return t[0]
    }

    private fun attach(tex: Int) {
        GLES20.glFramebufferTexture2D(
            GLES20.GL_FRAMEBUFFER, GLES20.GL_COLOR_ATTACHMENT0, GLES20.GL_TEXTURE_2D, tex, 0)
        if (GLES20.glCheckFramebufferStatus(GLES20.GL_FRAMEBUFFER) !=
            GLES20.GL_FRAMEBUFFER_COMPLETE)
            throw IllegalStateException("framebuffer incomplete")
    }

    private fun setOesUniforms(plain: Boolean) {
        GLES20.glUseProgram(program)
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, texId)
        GLES20.glUniform1i(uTexture, 0)
        GLES20.glUniformMatrix4fv(uTex, 1, false, matrix, 0)
        GLES20.glUniform2f(uSrc, sourceWidth.toFloat(), sourceHeight.toFloat())
        GLES20.glUniform2f(uParams, if (plain) 0f else 2f, strength)
    }

    // FSR: OES frame -> RGB texture (video size) -> EASU (screen size) -> RCAS (to the window)
    private fun drawFsr(w: Int, h: Int) {
        if (rgbTex == 0 || rgbW != sourceWidth || rgbH != sourceHeight) {
            rgbTex = makeTexture(rgbTex, sourceWidth, sourceHeight)
            rgbW = sourceWidth
            rgbH = sourceHeight
        }
        if (outTex == 0 || outW != w || outH != h) {
            outTex = makeTexture(outTex, w, h)
            outW = w
            outH = h
        }
        GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, fbo)
        attach(rgbTex)
        GLES20.glViewport(0, 0, sourceWidth, sourceHeight)
        setOesUniforms(true)
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)
        // EASU
        attach(outTex)
        GLES20.glViewport(0, 0, w, h)
        GLES20.glUseProgram(easuProgram)
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, rgbTex)
        GLES20.glUniform1i(easuInput, 0)
        val iw = sourceWidth.toFloat()
        val ih = sourceHeight.toFloat()
        val ow = w.toFloat()
        val oh = h.toFloat()
        GLES30.glUniform4ui(
            easuCon[0],
            bits(iw / ow),
            bits(ih / oh),
            bits(0.5f * iw / ow - 0.5f),
            bits(0.5f * ih / oh - 0.5f))
        GLES30.glUniform4ui(easuCon[1], bits(1f / iw), bits(1f / ih), bits(1f / iw), bits(-1f / ih))
        GLES30.glUniform4ui(easuCon[2], bits(-1f / iw), bits(2f / ih), bits(1f / iw), bits(2f / ih))
        GLES30.glUniform4ui(easuCon[3], 0, bits(4f / ih), 0, 0)
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)
        // RCAS to the window; 0 stops = sharpest (AMD's default is about 0.2)
        GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, 0)
        GLES20.glViewport(0, 0, w, h)
        GLES20.glUseProgram(rcasProgram)
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, outTex)
        GLES20.glUniform1i(rcasInput, 0)
        val sharp = Math.pow(2.0, -0.25).toFloat()
        val hb = half(sharp)
        GLES30.glUniform4ui(rcasCon, bits(sharp), hb or (hb shl 16), 0, 0)
        GLES30.glUniform2i(rcasSz, w, h)
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)
    }

    // runs on the renderer thread, once per decoded frame
    private fun draw() {
        if (released || broken) return
        try {
            val st = surfaceTexture ?: return
            st.updateTexImage()
            st.getTransformMatrix(matrix)
            EGL14.eglQuerySurface(display, eglSurface, EGL14.EGL_WIDTH, size, 0)
            val w = size[0]
            EGL14.eglQuerySurface(display, eglSurface, EGL14.EGL_HEIGHT, size, 0)
            val h = size[0]
            if (w <= 0 || h <= 0) return
            val enlarging = w >= sourceWidth * 1.05f && h >= sourceHeight * 1.05f
            val fsr = enlarging && mode == 1 && fsrOk
            if (fsr) drawFsr(w, h)
            else {
                GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, 0)
                GLES20.glViewport(0, 0, w, h)
                setOesUniforms(!enlarging)
                GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)
            }
            val e = GLES20.glGetError()
            if (e != 0) {
                if (fsr) {
                    fsrOk = false
                    onLog(
                        "Upscale: FSR stopped (GL error 0x${Integer.toHexString(e)}) - the cubic filter is used")
                } else if (++glErrors > 3)
                    throw IllegalStateException("GL error 0x${Integer.toHexString(e)}")
            }
            if (!EGL14.eglSwapBuffers(display, eglSurface))
                throw IllegalStateException(
                    "eglSwapBuffers 0x${Integer.toHexString(EGL14.eglGetError())}")
            if (++frames == 1L)
                onLog(
                    "Upscale: first frame drawn at ${w}x$h (video ${sourceWidth}x$sourceHeight, ${if (!enlarging) "plain" else if (fsr) "FSR 1" else "cubic"})")
        } catch (t: Throwable) {
            broken = true
            onLog(
                "Upscale: drawing stopped (${t.javaClass.simpleName}: ${t.message}) after $frames frames")
        }
    }

    private fun teardown() {
        try {
            if (display != EGL14.EGL_NO_DISPLAY) {
                EGL14.eglMakeCurrent(
                    display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT)
                if (eglSurface != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(display, eglSurface)
                if (context != EGL14.EGL_NO_CONTEXT) EGL14.eglDestroyContext(display, context)
                EGL14.eglTerminate(display)
            }
        } catch (_: Throwable) {}
        try {
            if (::inputSurface.isInitialized) inputSurface.release()
        } catch (_: Throwable) {}
        try {
            surfaceTexture?.release()
        } catch (_: Throwable) {}
        display = EGL14.EGL_NO_DISPLAY
        context = EGL14.EGL_NO_CONTEXT
        eglSurface = EGL14.EGL_NO_SURFACE
    }

    // Call after the decoder has been released.
    fun release() {
        if (released) return
        released = true
        val latch = CountDownLatch(1)
        if (handler.post {
            try {
                teardown()
            } finally {
                latch.countDown()
            }
        })
            latch.await(2, TimeUnit.SECONDS)
        thread.quitSafely()
        onLog("Upscale: released after $frames frames")
    }

    private companion object {
        const val VERTEX =
            """
            attribute vec2 aPos;
            uniform mat4 uTex;
            varying vec2 vUv;
            void main() {
                gl_Position = vec4(aPos, 0.0, 1.0);
                vUv = (uTex * vec4(aPos.x * 0.5 + 0.5, aPos.y * 0.5 + 0.5, 0.0, 1.0)).xy;
            }
        """
        const val FRAGMENT =
            """
            #extension GL_OES_EGL_image_external : require
            precision highp float;
            uniform samplerExternalOES uTexture;
            uniform vec2 uSrc;
            uniform vec2 uParams;   // x: 0 plain, 1 Catmull-Rom, 2 Catmull-Rom + sharpen; y: strength 0..1
            varying vec2 vUv;
            vec3 cr(vec2 q) {
                vec2 pos = q * uSrc;
                vec2 c = floor(pos - 0.5) + 0.5;
                vec2 f = pos - c;
                vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
                vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
                vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
                vec2 w3 = f * f * (-0.5 + 0.5 * f);
                vec2 w12 = w1 + w2;
                vec2 t0 = (c - 1.0) / uSrc;
                vec2 t12 = (c + w2 / w12) / uSrc;
                vec2 t3 = (c + 2.0) / uSrc;
                vec3 r = vec3(0.0);
                r += texture2D(uTexture, vec2(t0.x, t0.y)).rgb * (w0.x * w0.y);
                r += texture2D(uTexture, vec2(t12.x, t0.y)).rgb * (w12.x * w0.y);
                r += texture2D(uTexture, vec2(t3.x, t0.y)).rgb * (w3.x * w0.y);
                r += texture2D(uTexture, vec2(t0.x, t12.y)).rgb * (w0.x * w12.y);
                r += texture2D(uTexture, vec2(t12.x, t12.y)).rgb * (w12.x * w12.y);
                r += texture2D(uTexture, vec2(t3.x, t12.y)).rgb * (w3.x * w12.y);
                r += texture2D(uTexture, vec2(t0.x, t3.y)).rgb * (w0.x * w3.y);
                r += texture2D(uTexture, vec2(t12.x, t3.y)).rgb * (w12.x * w3.y);
                r += texture2D(uTexture, vec2(t3.x, t3.y)).rgb * (w3.x * w3.y);
                return r;
            }
            void main() {
                if (uParams.x < 0.5) { gl_FragColor = vec4(texture2D(uTexture, vUv).rgb, 1.0); return; }
                vec3 c = cr(vUv);
                if (uParams.x > 1.5) {
                    vec2 px = 1.0 / uSrc;
                    vec3 c0 = texture2D(uTexture, vUv).rgb;
                    vec3 n = texture2D(uTexture, vUv + vec2(0.0, -px.y)).rgb;
                    vec3 s = texture2D(uTexture, vUv + vec2(0.0, px.y)).rgb;
                    vec3 e = texture2D(uTexture, vUv + vec2(px.x, 0.0)).rgb;
                    vec3 w = texture2D(uTexture, vUv + vec2(-px.x, 0.0)).rgb;
                    vec3 lo = min(c0, min(min(n, s), min(e, w)));
                    vec3 hi = max(c0, max(max(n, s), max(e, w)));
                    vec3 blur = (n + s + e + w + c0 * 2.0) / 6.0;
                    c = clamp(c + uParams.y * 1.5 * (c - blur), lo - 0.03, hi + 0.03);
                }
                gl_FragColor = vec4(clamp(c, 0.0, 1.0), 1.0);
            }
        """

        // ---- FSR (GLSL ES 3.00); texelFetch stands in for textureGather, which ES 3.00 lacks ----
        const val FSR_VERTEX =
            "#version 300 es\nlayout(location=0) in vec2 aPos;\nvoid main() { gl_Position = vec4(aPos, 0.0, 1.0); }\n"
        const val FSR_HEAD =
            "#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\n#define A_GPU 1\n#define A_GLSL 1\n"
        const val EASU_MID =
            """
#define FSR_EASU_F 1
uniform sampler2D InputTexture;
uniform uvec4 con0;
uniform uvec4 con1;
uniform uvec4 con2;
uniform uvec4 con3;
vec4 gather4(vec2 p, int ch) {
  ivec2 sz = textureSize(InputTexture, 0);
  ivec2 b = ivec2(floor(p * vec2(sz) - 0.5));
  ivec2 mx = sz - 1;
  vec4 t00 = texelFetch(InputTexture, clamp(b, ivec2(0), mx), 0);
  vec4 t10 = texelFetch(InputTexture, clamp(b + ivec2(1, 0), ivec2(0), mx), 0);
  vec4 t01 = texelFetch(InputTexture, clamp(b + ivec2(0, 1), ivec2(0), mx), 0);
  vec4 t11 = texelFetch(InputTexture, clamp(b + ivec2(1, 1), ivec2(0), mx), 0);
  if (ch == 0) return vec4(t01.r, t11.r, t10.r, t00.r);
  if (ch == 1) return vec4(t01.g, t11.g, t10.g, t00.g);
  return vec4(t01.b, t11.b, t10.b, t00.b);
}
AF4 FsrEasuRF(AF2 p) { return gather4(p, 0); }
AF4 FsrEasuGF(AF2 p) { return gather4(p, 1); }
AF4 FsrEasuBF(AF2 p) { return gather4(p, 2); }
"""
        const val EASU_TAIL =
            """
layout(location=0) out highp vec4 o;
void main() { AF3 c; FsrEasuF(c, AU2(gl_FragCoord.xy), con0, con1, con2, con3); o = vec4(c, 1.0); }
"""
        const val RCAS_MID =
            """
#define FSR_RCAS_F 1
uniform sampler2D InputTexture;
uniform uvec4 con;
uniform ivec2 sz;
AF4 FsrRcasLoadF(ASU2 p) { return texelFetch(InputTexture, clamp(ivec2(p), ivec2(0), sz - 1), 0); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
"""
        const val RCAS_TAIL =
            """
layout(location=0) out highp vec4 o;
void main() { ivec2 q = ivec2(gl_FragCoord.xy); AF1 r, g, b; FsrRcasF(r, g, b, AU2(q), con); o = vec4(r, g, b, 1.0); }
"""
    }
}
