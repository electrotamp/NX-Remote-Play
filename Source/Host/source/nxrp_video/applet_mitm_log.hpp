/*
 * applet-mitm - Track B
 * Batched SD-card logger (libstratosphere fs) and latest-step breadcrumb.
 *
 *   sdmc:/config/nxremoteplay/host_log.txt   bounded background batches
 *   sdmc:/config/nxremoteplay/host_last.txt  single line: the step currently in flight
 *
 * Sudden power loss can lose the pending batch. Explicit aborts flush it first.
 */
#pragma once
#include <stratosphere.hpp>
#include <atomic>

namespace ams::mitm::applet
{

/* Live counters. The heartbeat snapshots these into .last, so even a run
 * that ends in a forced power-off (which can lose the .log tail before FAT
 * commits) still tells us how far the module got: did it ever get a
 * session, did the wrapper chain run, were frames flowing. */
struct Stats
{
    std::atomic<u32> sessions; /* OnNeedsToAccept calls   */
    std::atomic<u32> getdisp;  /* GetDisplayService calls */
    std::atomic<u32> relay;    /* GetRelayService calls   */
    std::atomic<u32> txns;     /* binder transactions     */
};
extern Stats g_stats;

void LogInit();
void LogFlush(); // Blocking: explicit fatal paths only, never the media loop.
/* always true in this build: the log is sdmc:/config/nxremoteplay/host_log.txt (capped) */
void LogLine(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* true holds lines and suppresses breadcrumbs; false queues the held lines. */
void LogDefer(bool on);

/* the user-facing support file sdmc:/config/nxremoteplay/host_support.txt (see applet_mitm_log.cpp). SupportBoot
 * writes its header (console, firmware, Atmosphere, files, memory) once at start; SupportLine adds a plain line (never
 * passwords, IDs or IPs); SupportPools adds a memory snapshot. LogLine also copies the lines that matter. */
void SupportBoot(const char *build);
void SupportLine(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void SupportPools(const char *when);

/* An explicit abort drains pending diagnostics before showing the fatal screen.
 * Only once the log is open: sm::Initialize, before it, cannot use it. */
#define NXRP_ABORT_UNLESS(expr) do {                                                                   \
        const ::ams::Result nxrp_abort_rc_ = (expr);                                                       \
        if (R_FAILED(nxrp_abort_rc_)) {                                                                    \
            ::ams::mitm::applet::LogDefer(false);                                                       \
            ::ams::mitm::applet::LogFlush();                                                            \
            ::ams::mitm::applet::LogLine("FATAL: %s failed, rc=0x%x (%s:%d) - stopping the console here", \
                                         #expr, nxrp_abort_rc_.GetValue(), __FILE__, __LINE__);            \
            ::ams::mitm::applet::LogFlush();                                                            \
            R_ABORT_UNLESS(nxrp_abort_rc_);                                                                \
        }                                                                                                 \
    } while (0)

/* Queue the latest breadcrumb and LogLine("-> what"). */
void LogMark(const char *what);

/*  - USB bulk transport. Implemented in applet_mitm_main.cpp, where the
 * usb:ds session and the endpoint handles live. Declared here because that
 * file keeps its USB state in an anonymous namespace, which has internal
 * linkage and cannot be reached by an extern declaration from another
 * translation unit - the lesson LogMemoryPools taught in .
 *
 * UsbReady asks the kernel for the live UsbState rather than caching a
 * boot-time flag, so a cable seated after boot still counts.
 *
 * The buffer passed to UsbSendBuffer MUST be 0x1000-aligned and in normal
 * CACHED memory. Uncached nvmap memory is rejected (the 0xd401
 * InvalidCurrentMemory class of failure); g_ind_buf satisfies both. */
bool UsbReady();
bool UsbSendBuffer(const void *buf, size_t len, size_t *out_sent);

/*  - asynchronous single-URB post, so a frame's transfer overlaps the
 * next frame's capture. Serially, 480x270 costs 5.6 ms read + 12.4 ms send
 * = 43 fps; overlapped the bottleneck is the send alone and 60 fps fits.
 * Same buffer rules as UsbSendBuffer: 0x1000-aligned, cached memory.
 * The buffer must stay untouched until UsbWaitAsync returns. */
bool UsbPostAsync(const void *buf, size_t len, u32 *out_urb);
bool UsbWaitAsync(u32 urb, size_t *out_sent);
/* the viewer says goodbye ("SFTQ" on the OUT endpoint) when it
 * closes, so a stream ends at once instead of after the 5 s timeout.
 * Called from the stream loop; UsbByeStop at the end of a session. */
bool UsbViewerSaidBye();
void UsbByeStop();

/* true if a host program is reading the stream endpoint right now.
 * Sends one empty SFTR header (length 0, which raw-view skips) and waits
 * `timeout_ms` for it to be taken. A cable with no viewer open stays
 * Configured, so UsbReady alone cannot tell. */
bool UsbViewerPresent(u32 timeout_ms);
/* why the viewer gets no picture, sent to it after each idle
 * probe that finds it (a status packet old viewers ignore) - 0 while
 * streaming */
enum ViewerReason : u32
{
    ViewerReason_None = 0,
    ViewerReason_AppletOpen = 3,
    ViewerReason_Unsupported = 6,
    ViewerReason_Wedged = 7,
    ViewerReason_NoMemory = 8,
    ViewerReason_Starting = 10,
};
void SetViewerReason(ViewerReason r);
/* USB follows the stream switch (VIC worker thread only) */

/* ServerManager::LoopProcess aborts the whole module when one
 * request cannot be handled - accepting one session more than a manager
 * has room for is enough - and the module going down takes every vi
 * client with it: a black screen until the power button. This loop logs
 * it and carries on: that one client is turned away, the console keeps
 * going (BigBro030, GBAtemp: apps launched one after another). */
template <typename Manager> void LoopProcessSafely(Manager &m, const char *who)
{
    u32 failures = 0;
    while (auto *h = m.WaitSignaled())
    {
        if (const Result r = m.Process(h); R_FAILED(r))
        {
            if (++failures <= 20)
            {
                LogLine("%s: a request could not be handled (rc=0x%x) - turned away, still running", who, r.GetValue());
            }
        }
    }
}

} // namespace ams::mitm::applet
