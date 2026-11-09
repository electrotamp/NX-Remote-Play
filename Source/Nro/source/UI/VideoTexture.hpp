#pragma once
#include <cstdint>
#include "UI.hpp"

// Uploads decoded NV12 planes and draws them with a YUV-to-RGB shader.
// The playback page supplies frames from the hardware decoder; this class owns no network connection.
class VideoTexture
{
  public:
    VideoTexture() = default;
    ~VideoTexture();

    VideoTexture(const VideoTexture &) = delete;
    VideoTexture &operator=(const VideoTexture &) = delete;

    // Uploads one decoded NV12 frame: an 8-bit luma (Y) plane followed by an
    // interleaved 8-bit chroma (U,V) plane at half width/height - exactly
    // what av_hwframe_transfer_data() hands back for the sw_format=nv12
    // decoded frames. Strides
    // may exceed width/width*2 (FFmpeg pads rows) - handled via
    // GL_UNPACK_ROW_LENGTH, no manual row copy needed. Safe to call every
    // frame; textures are only (re)allocated when the dimensions change.
    void UploadNV12(const uint8_t *yPlane, int yStride, const uint8_t *uvPlane, int uvStride, int width, int height);

    // Draws the most recently uploaded frame at the given size, at the
    // current ImGui cursor position, via a custom shader submitted through
    // ImDrawList::AddCallback - every other widget's own rendering is
    // unaffected (see VideoTexture.cpp for exactly how that isolation
    // works). No-op until at least one UploadNV12() call has happened.
    void Draw(ImVec2 size);

    bool HasFrame() const
    {
        return width > 0 && height > 0;
    }

  private:
    GLuint yTex = 0, uvTex = 0;
    GLuint program = 0;
    GLint uniformProjMtx = -1, uniformYTex = -1, uniformUvTex = -1;
    int width = 0, height = 0;
    int textureWidth = 0, textureHeight = 0;

    void EnsureProgram();
    void EnsureTextures(int w, int h);

    static void BindShaderCallback(const ImDrawList *parentList, const ImDrawCmd *cmd);
    static void RestoreShaderCallback(const ImDrawList *parentList, const ImDrawCmd *cmd);
};
