// adapt the reference project's one-outstanding-send contract to our remote-play wire
// protocol. Network AU copies go straight to bounded viewer mailboxes; Wait means
// a participating sender completed its full socket write, not merely a FIFO copy.
// Raw TCP/USB diagnostic consumers retain the legacy capture-ring route.
#include "applet_mitm_log.hpp"
#include "applet_mitm_nv.hpp"
#include "../nxrp_video.h"
#include <cstring>

namespace ams::mitm::applet
{
namespace
{
struct WireHeader
{
    u32 magic;
    u16 version, flags;
    u32 width, height, stride, length, block_height, kind;
};
static_assert(sizeof(WireHeader) == 32);
size_t g_expected = 0, g_completed = 0;
u32 g_submitted = 0;
u64 g_timestamp = 0;
ViewerReason g_reason = ViewerReason_Starting;
} // namespace
bool UsbReady()
{
    return CaptureVideoViewerPresent();
}

bool UsbViewerPresent(u32 timeout_ms)
{
    for (u32 ms = 0; !CaptureVideoViewerPresent(); ms += 20)
    {
        if (ms >= timeout_ms)
            return false;
        os::SleepThread(TimeSpan::FromMilliSeconds(20));
    }
    return true;
}
bool UsbSendBuffer(const void *data, size_t size, size_t *sent)
{
    if (sent)
        *sent = 0;
    if (!data || size != sizeof(WireHeader) || !CaptureVideoViewerPresent())
        return false;
    WireHeader h;
    std::memcpy(&h, data, sizeof(h));
    // This boundary accepts H.264 only. Audio remains TCP transport's own GRC path.
    if (h.magic != 0x52544653 || h.version != 2 || !(h.flags & 2) || !h.length || h.length > 0x54000 ||
        h.width != 1280 || h.height != 720)
        return false;
    g_expected = h.length;
    const u64 now_us = armTicksToNs(armGetSystemTick()) / 1000;
    g_timestamp = now_us >= h.stride ? now_us - h.stride : now_us;
    if (sent)
        *sent = size;
    return true;
}
bool UsbPostAsync(const void *data, size_t size, u32 *urb)
{
    if (urb)
        *urb = 0;
    if (!g_expected || size != g_expected)
        return false;
    g_expected = 0;
    if (!CaptureVideoSubmit(data, size, g_timestamp))
        return false;
    g_completed = size;
    if (++g_submitted <= 4 || g_submitted % 300 == 0)
        LogLine("NX-Remote-Play video AU #%u: %zu bytes copied into transport queue, IDR=%d", g_submitted, size,
                CaptureVideoHasIdr(static_cast<const u8 *>(data), size));
    return true;
}
bool UsbWaitAsync(u32, size_t *sent)
{
    const bool ok = g_completed != 0 && CaptureVideoWaitDelivery();
    if (sent)
        *sent = ok ? g_completed : 0;
    g_completed = 0;
    return ok;
}
bool UsbViewerSaidBye()
{
    return !CaptureVideoViewerPresent();
}
void UsbByeStop()
{
    g_expected = g_completed = 0;
}



void SetViewerReason(ViewerReason r)
{
    if (r != g_reason)
        LogLine("NX-Remote-Play video source state %u", static_cast<u32>(r));
    g_reason = r;
}

// No second socket stack, USB owner, beacon or independent viewer protocol is started.









} // namespace ams::mitm::applet
