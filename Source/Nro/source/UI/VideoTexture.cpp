#include "VideoTexture.hpp"
#include "imgui/imgui_impl_opengl3.h"
#include <cstdio>

// --- Why this needs its own shader instead of ImGui::Image() -----------
// ImGui::Image() draws through this app's single existing shader (imgui's
// own RGBA sampler, imgui_impl_opengl3.cpp), which has nowhere to do a
// YUV->RGB conversion. This renderer draws a textured quad with its
// own program via ImDrawList::AddCallback, then hands control straight back
// to imgui's normal draw path for every widget after it in the same frame.
//
// That  only works because of two things imgui_impl_opengl3.cpp does
// NOT vary per draw command: it calls glUseProgram(g_ShaderHandle) exactly
// ONCE per RenderDrawData (not once per command), and it never touches
// GL_TEXTURE1. So: BindShaderCallback swaps the active program to this
// class's own and binds the chroma plane to unit 1 (imgui never looks at
// unit 1, so this is invisible to it); the actual quad geometry then comes
// from the very next command, an ordinary ImGui::Image() call using the
// LUMA texture's own GLuint as its ImTextureID, so imgui's unmodified
// per-command glBindTexture(GL_TEXTURE_2D, pcmd->TextureId) rebinds unit 0
// to exactly the texture this shader wants there anyway; RestoreShaderCallback
// then swaps the program back before the next widget draws, since nothing
// else will.
//
// The vertex attribute locations (Position/UV/Color) are NOT assumed to be
// 0/1/2 - imgui's GLSL-130 shader has no explicit layout qualifiers, so the
// driver assigns them, and this shader is bound to whatever imgui's actually
// got via ImGui_ImplOpenGL3_GetAttribLocations() (added for this) before
// linking, so the one shared VAO's vertex format stays valid for both
// programs without re-touching glVertexAttribPointer per draw.

namespace
{
const GLchar *VertexShaderSource = "#version 130\n"
                                   "uniform mat4 ProjMtx;\n"
                                   "in vec2 Position;\n"
                                   "in vec2 UV;\n"
                                   "in vec4 Color;\n"
                                   "out vec2 Frag_UV;\n"
                                   "out vec4 Frag_Color;\n"
                                   "void main()\n"
                                   "{\n"
                                   "    Frag_UV = UV;\n"
                                   "    Frag_Color = Color;\n"
                                   "    gl_Position = ProjMtx * vec4(Position.xy, 0, 1);\n"
                                   "}\n";

// BT.601 limited-range (16-235/16-240) YUV->RGB - matches this app's
// H.264 decoder's default NV12 output. Not yet verified against a real
// BT.709 game-capture source on hardware .
const GLchar *FragmentShaderSource = "#version 130\n"
                                     "uniform sampler2D YTex;\n"
                                     "uniform sampler2D UVTex;\n"
                                     "in vec2 Frag_UV;\n"
                                     "in vec4 Frag_Color;\n"
                                     "out vec4 Out_Color;\n"
                                     "void main()\n"
                                     "{\n"
                                     "    float y = (texture(YTex, Frag_UV.st).r - 0.0625) * 1.164383;\n"
                                     "    vec2 uv = texture(UVTex, Frag_UV.st).rg - vec2(0.5, 0.5);\n"
                                     "    float r = y + 1.596027 * uv.y;\n"
                                     "    float g = y - 0.391762 * uv.x - 0.812968 * uv.y;\n"
                                     "    float b = y + 2.017232 * uv.x;\n"
                                     "    Out_Color = Frag_Color * vec4(r, g, b, 1.0);\n"
                                     "}\n";

GLuint CompileShader(GLenum type, const GLchar *source, const char *desc)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint status = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status == GL_FALSE)
    {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        fprintf(stderr, "VideoTexture: failed to compile %s: %s\n", desc, log);
    }
    return shader;
}
} // namespace

void VideoTexture::EnsureProgram()
{
    if (program)
        return;

    GLuint vert = CompileShader(GL_VERTEX_SHADER, VertexShaderSource, "video vertex shader");
    GLuint frag = CompileShader(GL_FRAGMENT_SHADER, FragmentShaderSource, "video fragment shader");

    program = glCreateProgram();
    glAttachShader(program, vert);
    glAttachShader(program, frag);

    // Must match imgui's own runtime-assigned locations exactly - see this
    // file's top comment - not whatever the driver would otherwise pick for
    // this program on its own.
    GLint position = 0, uv = 0, color = 0;
    ImGui_ImplOpenGL3_GetAttribLocations(&position, &uv, &color);
    glBindAttribLocation(program, position, "Position");
    glBindAttribLocation(program, uv, "UV");
    glBindAttribLocation(program, color, "Color");

    glLinkProgram(program);

    GLint linkStatus = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linkStatus);
    if (linkStatus == GL_FALSE)
    {
        char log[512];
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        fprintf(stderr, "VideoTexture: failed to link shader program: %s\n", log);
    }

    glDetachShader(program, vert);
    glDetachShader(program, frag);
    glDeleteShader(vert);
    glDeleteShader(frag);

    uniformProjMtx = glGetUniformLocation(program, "ProjMtx");
    uniformYTex = glGetUniformLocation(program, "YTex");
    uniformUvTex = glGetUniformLocation(program, "UVTex");
}

void VideoTexture::EnsureTextures(int w, int h)
{
    if (yTex && textureWidth == w && textureHeight == h)
        return;

    if (!yTex)
    {
        glGenTextures(1, &yTex);
        glGenTextures(1, &uvTex);
    }

    glBindTexture(GL_TEXTURE_2D, yTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);

    glBindTexture(GL_TEXTURE_2D, uvTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG, w / 2, h / 2, 0, GL_RG, GL_UNSIGNED_BYTE, nullptr);

    glBindTexture(GL_TEXTURE_2D, 0);

    textureWidth = w;
    textureHeight = h;
}

void VideoTexture::UploadNV12(const uint8_t *yPlane, int yStride, const uint8_t *uvPlane, int uvStride, int w, int h)
{
    if (w <= 0 || h <= 0 || !yPlane || !uvPlane)
        return;

    EnsureTextures(w, h);

    GLint previousUnpackRowLength = 0;
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &previousUnpackRowLength);

    glBindTexture(GL_TEXTURE_2D, yTex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, yStride);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RED, GL_UNSIGNED_BYTE, yPlane);

    glBindTexture(GL_TEXTURE_2D, uvTex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, uvStride / 2);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w / 2, h / 2, GL_RG, GL_UNSIGNED_BYTE, uvPlane);

    glPixelStorei(GL_UNPACK_ROW_LENGTH, previousUnpackRowLength);
    glBindTexture(GL_TEXTURE_2D, 0);

    width = w;
    height = h;
}

void VideoTexture::BindShaderCallback(const ImDrawList *, const ImDrawCmd *cmd)
{
    VideoTexture *self = (VideoTexture *)cmd->UserCallbackData;

    glUseProgram(self->program);

    ImDrawData *drawData = ImGui::GetDrawData();
    float L = drawData->DisplayPos.x;
    float R = drawData->DisplayPos.x + drawData->DisplaySize.x;
    float T = drawData->DisplayPos.y;
    float B = drawData->DisplayPos.y + drawData->DisplaySize.y;
    const float orthoProjection[4][4] = {
        {2.0f / (R - L), 0.0f, 0.0f, 0.0f},
        {0.0f, 2.0f / (T - B), 0.0f, 0.0f},
        {0.0f, 0.0f, -1.0f, 0.0f},
        {(R + L) / (L - R), (T + B) / (B - T), 0.0f, 1.0f},
    };
    glUniformMatrix4fv(self->uniformProjMtx, 1, GL_FALSE, &orthoProjection[0][0]);
    glUniform1i(self->uniformYTex, 0);
    glUniform1i(self->uniformUvTex, 1);

    // Unit 0 is left alone here - the ImGui::Image() draw command right
    // after this callback already rebinds it to the luma texture via its
    // own ImTextureID (see this file's top comment).
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, self->uvTex);
    glActiveTexture(GL_TEXTURE0);
}

void VideoTexture::RestoreShaderCallback(const ImDrawList *, const ImDrawCmd *)
{
    glUseProgram(ImGui_ImplOpenGL3_GetShaderHandle());
}

void VideoTexture::Draw(ImVec2 size)
{
    if (!HasFrame())
        return;

    EnsureProgram();

    ImDrawList *drawList = ImGui::GetWindowDrawList();
    drawList->AddCallback(&VideoTexture::BindShaderCallback, this);
    ImGui::Image((ImTextureID)(intptr_t)yTex, size);
    drawList->AddCallback(&VideoTexture::RestoreShaderCallback, this);
}

VideoTexture::~VideoTexture()
{
    if (yTex)
        glDeleteTextures(1, &yTex);
    if (uvTex)
        glDeleteTextures(1, &uvTex);
    if (program)
        glDeleteProgram(program);
}
