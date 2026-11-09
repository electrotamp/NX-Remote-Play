#pragma once
#include <cstdint>
#include <cstddef>

// Continuous H.264 decode via switch-ffmpeg's NVTEGRA hardware backend -
// the same decode+CPU-transfer path DecodeProbe proved on real hardware
// (, one-shot, on a bundled test clip - see the retained decoder test record).
// This is that same proven call sequence, kept running frame after frame
// instead of once, and fed by the receiver's reassembled access
// units instead of a static file.
//
// Real Switch NRO build only (needs switch-ffmpeg/NVTEGRA, guarded the same
// way the Makefile itself gates FFmpeg linking - see the .cpp). On other
// platforms this is a harmless no-op: DecodeAccessUnit always returns
// false, nothing crashes, nothing is expected to render.
class H264StreamDecoder
{
  public:
    H264StreamDecoder() = default;
    ~H264StreamDecoder();

    H264StreamDecoder(const H264StreamDecoder &) = delete;
    H264StreamDecoder &operator=(const H264StreamDecoder &) = delete;

    // Decodes one Annex-B access unit from the receiver.
    // On success, calls onFrame with the decoded frame's NV12 planes -
    // synchronously, before returning, since av_hwframe_transfer_data's
    // result is only valid for the duration of this call (the same
    // short-lived AVFrame is reused every call). Returns false if decode
    // produced no displayable frame this call (normal for the first few
    // packets - decoders buffer) or on error.
    // present=false : decode only - the frame goes through the decoder (it is a reference for
    // the next P-frame) but is NOT transferred to the CPU or handed to onFrame. Used for the older frames
    // of a backlog so catching up costs only the decode.
    bool DecodeAccessUnit(const uint8_t *data, size_t size,
                          void (*onFrame)(const uint8_t *yPlane, int yStride, const uint8_t *uvPlane, int uvStride,
                                          int width, int height, void *userdata),
                          void *userdata, bool present = true);

    // True once if a packet was rejected or the decoder reported an error since the last call: its
    // references can no longer be trusted, so the caller must restart on the next keyframe.
    bool TakeError();

    // Drops everything the decoder holds (call before restarting on a keyframe after an error).
    void Flush();

  private:
    void *codecContext = nullptr; // AVCodecContext*
    void *hwDeviceRef = nullptr;  // AVBufferRef*
    void *packet = nullptr;       // AVPacket*
    void *hwFrame = nullptr;      // AVFrame* (decoder output, still hw-resident)
    void *swFrame = nullptr;      // AVFrame* (after av_hwframe_transfer_data, CPU-readable)
    bool initFailed = false;
    bool hadError = false;

    bool EnsureInitialized();
};
