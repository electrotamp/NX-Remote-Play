#include "ConnectorMedia.hpp"
#include "RendezvousClient.hpp"
#include "H264StreamDecoder.hpp"
#include "../UI/Image.hpp"
#include <vector>

#include <switch.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>

namespace ConnectorMedia
{
namespace
{
H264StreamDecoder decoder;
VideoTexture videoTexture;
bool registered = false;

// HOME/unfocused still-frame relay  - separate registration
// flag/state from the video callback above, since a caller may want one
// without the other in principle, and it keeps EnsureStillFrameCallbackRegistered()
// idempotent the same way EnsureVideoCallbackRegistered() already is.
bool stillRegistered = false;
Image::Img stillImage;
bool haveStillImage = false;
// false while Poll() is feeding the OLDER frames of a backlog: they are decoded (references for the
// following P-frames) but not transferred/uploaded/shown. Only the newest frame of a batch is presented.
bool presentFrames = true;

// Which source delivered the NEWEST picture : the playback scene used to prefer live
// video whenever the H.264 texture had EVER received a frame, so after any video frame (e.g. black
// frames from a game or before HOME capture worked) the newer HOME stills were never shown.
unsigned long long mediaSeq = 0, videoSeq = 0, stillSeq = 0;

void OnDecodedFrame(const uint8_t *yPlane, int yStride, const uint8_t *uvPlane, int uvStride, int width, int height,
                    void *)
{
    videoTexture.UploadNV12(yPlane, yStride, uvPlane, uvStride, width, height);
    videoSeq = ++mediaSeq;
}

// Hoisted from OnAccessUnit's function-local statics so ResetVideoSync() can clear them.
bool synced = false;
bool paramsFed = false;
bool receivedWhileUnsynced = false;
unsigned decoderErrors = 0;
SyncStats syncStats;

// A rejected packet or decoder error leaves later P-frames without valid references: start over.
void RecoverFromDecoderError()
{
    if (!decoder.TakeError())
        return;
    ++decoderErrors;
    decoder.Flush();
    synced = false;
    paramsFed = false;
}

void OnAccessUnit(const uint8_t *data, size_t size, void *)
{
    // Only start (or resume after packet loss) decoding on a self-contained
    // keyframe AU: SPS(7)+PPS(8)+IDR(5). Feeding P-frames without references to
    // the nvtegra decoder crashes the NRO (null deref in nvtegra_fdd_priv_free).
    if (!synced)
    {
        receivedWhileUnsynced = true;
        bool sps = false, pps = false, idr = false, slice = false;
        // 00 00 01 also matches the tail of a four-byte start code, so both forms are found.
        for (size_t i = 0; i + 3 < size; i++)
        {
            if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            {
                uint8_t t = data[i + 3] & 0x1F;
                sps |= t == 7;
                pps |= t == 8;
                idr |= t == 5;
                slice |= t == 1;
            }
        }
        ++syncStats.unsyncedAus;
        syncStats.withSps += sps;
        syncStats.withPps += pps;
        syncStats.withIdr += idr;
        syncStats.withSlice += slice;
        syncStats.lastSize = size;
        // The Host's SPS/PPS have an older RTP timestamp than the IDR that follows,
        // so they usually arrive as their own AU - feed those (harmless, no frame
        // output) and remember, then sync on the IDR AU.
        if ((sps || pps) && !idr && !slice)
        {
            decoder.DecodeAccessUnit(data, size, &OnDecodedFrame, nullptr, presentFrames);
            paramsFed = true;
            return;
        }
        if (!idr || !(paramsFed || (sps && pps)))
            return;
        synced = true;
        ++syncStats.syncs;
        receivedWhileUnsynced = false;
    }
    decoder.DecodeAccessUnit(data, size, &OnDecodedFrame, nullptr, presentFrames);
    RecoverFromDecoderError();
}

// One H.264 NAL/access unit from the punched NxRemotePlay stream (Annex-B, SPS/PPS injected on
// keyframes by the console).
void OnPunchAccessUnit(uint8_t *data, size_t size, void *)
{
    OnAccessUnit(data, size, nullptr);
}

// HOME/unfocused still-frame relay . Runs on the same thread
// as the connector poll, called synchronously from the playback page each
// frame), so this OpenGL texture upload is safe without any locking.
// Image::Img's constructor throws on a decode failure (Image.cpp: null id/
// x/y) - guarded here rather than left to propagate, since a genuinely
// corrupt/truncated JPEG must never take the whole app down (main.cpp's
// top-level try/catch would otherwise treat it as a fatal, log-and-exit
// condition for what's really just one bad frame to skip).
void OnStillFrameBytes(uint8_t *data, size_t size, void *)
{
    try
    {
        stillImage = Image::Img(std::vector<u8>(data, data + size));
        haveStillImage = true;
        stillSeq = ++mediaSeq;
    }
    catch (...)
    {
        // Keep showing the last good still (or none) rather than crash.
    }
}
} // namespace

void EnsureVideoCallbackRegistered()
{
    if (registered)
        return;
    registered = true;
    // TCP punch stream: access units arrive ready-made (no RTP depacketizing needed).
    Network::Rendezvous::Connector::SetOnVideoFrame(&OnPunchAccessUnit, nullptr);
}

void SetPresentFrames(bool present)
{
    presentFrames = present;
}

void ResetVideoSync()
{
    synced = false;
    paramsFed = false;
    receivedWhileUnsynced = false;
}

bool WaitingForKeyframe()
{
    return !synced && receivedWhileUnsynced;
}

SyncStats TakeSyncStats()
{
    const SyncStats out = syncStats;
    syncStats = SyncStats{};
    return out;
}

unsigned TakeDecoderErrors()
{
    const unsigned n = decoderErrors;
    decoderErrors = 0;
    return n;
}

VideoTexture &GetVideoTexture()
{
    return videoTexture;
}

void EnsureStillFrameCallbackRegistered()
{
    if (stillRegistered)
        return;
    stillRegistered = true;
    Network::Rendezvous::Connector::SetOnStillFrame(&OnStillFrameBytes, nullptr);
}

void DrawStillFrame(ImVec2 size)
{
    if (!haveStillImage)
        return;
    ImGui::Image((ImTextureID)stillImage, size);
}

} // namespace ConnectorMedia

// ---- newest-source selection  --------------------------------------
namespace ConnectorMedia
{
bool ShowVideo()
{
    return videoSeq > stillSeq && videoTexture.HasFrame();
}
bool ShowStill()
{
    return haveStillImage && stillSeq > videoSeq;
}

void NewSession()
{
    haveStillImage = false;
    mediaSeq = videoSeq = stillSeq = 0;
    ResetVideoSync();
}
} // namespace ConnectorMedia
