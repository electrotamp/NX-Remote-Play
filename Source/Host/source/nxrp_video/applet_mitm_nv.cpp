/*
 * applet-mitm - hand-rolled nvdrv access + VIC hardware blit.
 *
 * Stage 1 (M4b..M7d, verified on hardware): the sequence below hand-rolls the
 * nvdrv IPC (libnx's nvInitialize() fatals a sysmodule), imports the game's
 * swapchain nvmap, surveys engines, allocates our own linear buffer, and shows
 * the VIC channel is usable. It then RELEASES everything - holding an nvdrv
 * session wedged homebrew.
 *
 * Stage 2 (M8, this file): TryVicBlit() does the real thing. One
 * NVHOST_IOCTL_CHANNEL_SUBMIT that kicks the VIC to de-swizzle + format-convert
 * a crop of the live frame (block-linear, kind 0xFE) into a linear buffer we
 * own, then waits the syncpoint and checksums the result. No MAP_CMD_BUFFER
 * (it crashed nvservices); the reloc list lets the kernel pin per-submit.
 *
 * nvdrv IPC (libnx nx/source/services/nv.c):
 *   cmd 0 Open      : InBuffer(MapAlias) = path -> {u32 fd, u32 error}
 *   cmd 1 Ioctl     : in {u32 fd, u32 request}, out u32 error, AutoSelect in+out = argp
 *   cmd 2 Close     : in {u32 fd}, out u32 error
 *   cmd 3 Initialize: in u32 transfermem_size, handles {process, tmem}
 *
 * CHANNEL_SUBMIT arg layout (switchbrew NV_services, libnx nvchannel.c):
 *   u32 num_cmdbufs, num_relocs, num_syncpt_incrs, num_fences;
 *   cmdbuf      cmdbufs[n]      {u32 mem, offset, words}                 (12)
 *   reloc       relocs[n]       {u32 cmdbuf_mem, cmdbuf_offset, target, target_offset} (16)
 *   reloc_shift reloc_shifts[n] {u32 shift}                             (4)
 *   syncpt_incr syncpt_incrs[n] {u32 syncpt_id, syncpt_incrs, rsvd[3]}  (20)
 *   u32         fence_thresholds[n]  (out)                              (4)
 * request = _NV_IOWR(0, 0x01, sizeof) -> 0xC0??0001 with ?? = total size.
 */
#include "applet_mitm_nv.hpp"
#include "../nxrp_bitrate.hpp"
#include "../nxrp_video.h"
#include "../nxrp_source.h"
#include "../nxrp_layers.h"
#include "applet_mitm_sm.hpp"
#include "applet_mitm_log.hpp"
#include "applet_mitm_dmdbg.hpp"
#include "hybrid_focus.hpp"
#include "vic40_config.hpp"
#include "nvenc_drv_h264.h"
#include "applet_mitm_clk.hpp"
#include "nvenc_grc_idr.h"
#include "nvenc_grc_hdrs.h"
#include "vic_csc_bt709.h"
#include "nvenc_grc_p.h"
#include "applet_mitm_dbgpump.hpp"
#include "applet_mitm_donor.hpp"
#include "applet_mitm_slotmatch.hpp"
#include <atomic>
#include <cstring>
#include <cstdio>

namespace ams::mitm::applet
{

constinit GameSurface g_game_surface = {};
constinit GameSurface g_applet_surface = {};
constinit std::atomic<u64> g_applet_target_pid{0};
constinit std::atomic<u64> g_present_pid{0};
constinit std::atomic<u64> g_app_present_tick{0};
/* the record a running session's geometry came from: a new buffer set
 * there restarts it (the game's, or  the Album's / hbmenu's) */
constinit const GameSurface *g_session_surface = &g_game_surface;
constinit u32 g_pmdmnt_rc = 0xFFFFFFFF;

/*  streaming prototype. 480x270 default: the only size that fits 60 fps
 * on USB 2.0 (518,400 B/frame = 12.4 ms transfer against a 16.67 ms budget).
 * 640x360 is selectable but caps near 45 fps until NV12 or SuperSpeed. */
constinit u32 g_nvstream_qp = 20;
constinit u32 g_nvstream_gop = 0;
/* Default on: release the debug attach when the game stops presenting so
 * HOME / Album is not fighting an active capture session. Set
 * detach_on_home=0 (arm "nodethome") to keep showing the suspended game. */

namespace
{

constinit std::atomic<bool> g_blit_done{false};

/* Static so the allocator is never in the path. : it must be
 * our own memory - it cannot live in the donor's (applet_mitm_donor):
 * transfer memory cannot be made from memory that is itself lent
 * (vb:2_tmem failed, test10). */
/* nvdrv's transfer memory (256 KB) comes from the donor with
 * its heap (nxrp command 12) - it was .bss, in the System group */

/* VIC buffers live on the REAL process heap, not in .bss.
 *
 * nvmap ALLOC binds the caller's pages, but a device-shared buffer also
 * has to have its CPU mapping marked uncached - libnx's nvMapCreate does
 * armDCacheFlush + svcSetMemoryAttribute(.., 8, 8) for exactly this. That
 * SVC is only permitted on MemoryState_Normal, i.e. heap; a sysmodule's
 * .bss is code-mutable and it would be rejected. Every previous run had
 * cached, non-shared .bss standing in for device memory, which is why the
 * engine reported OP_DONE and we read back nothing. */
constexpr size_t VicCfgSize = 0x4000; /* VicConfigStruct (1552 B) */
constexpr size_t VicCmdSize = 0x1000; /* host1x pushbuf           */
/* 1 MB, sized for a streamed frame rather than the 64x64 self-blit.
 * 640x360 RGBA is 921,600 B; 480x270 is 518,400. The old 0x10000 held
 * neither. */
/* 3.5 MB so a full 1920x1080 NV12 frame (3,110,400 B) fits, which
 * is both the widest useful VIC output and exactly what NVENC wants as
 * input. */
constexpr size_t VicDstSize = 0x340000; /* linear output            */
constexpr size_t VicSrcSize = 0x10000;  /* a SOURCE WE OWN, for the self-blit */
/* A fixed 8 MB request was refused with os::ResultOutOfMemory (0x1003):
 * a sysmodule's heap is capped well below that. Ask for the largest the
 * process will actually grant rather than guessing, and size the capture
 * to whatever we get. 2 MB granularity is the AllocateMemoryBlock unit. */
constexpr size_t VicBufsEnd = 0x360000; /* cfg+cmd+dst+src all live below this */
/* the dmnt:cht bounce used to be 2 MB of .bss for the whole
 * boot, in the Applet pool the browser applets need nearly all of
 * (2210-1024); it now sits here, taken when a viewer first connects.
 * Not nvmap-mapped (it must not be). */
constexpr size_t VicBounceOff = 0x400000; /* 2 MB: dmnt:cht read bounce */
constexpr size_t VicBounceSize = 0x200000;
/* the heap held once streaming has started - engine buffers and the above */
constexpr size_t VicBaseHeap = VicBounceOff + VicBounceSize;
static_assert(VicBufsEnd <= VicBounceOff);
/*  RELAYOUT - the blocker  predicted. Staging used to start
 * 983,040 B into the capture region, which collided with a resident
 * full frame (8,294,400 B). It now starts past a whole slot. */
constexpr size_t FbSlotBytes = 8847360;
constexpr size_t FbBlockRowStage = FbSlotBytes;
/* 0x220000 so a 960x540 frame (2,073,600 B) fits. Ceiling is the
 * 16 MB heap: VicBufsEnd + FbSlotBytes + 3 * StreamStageSize must stay
 * under g_ind_size (15,597,568), which caps this at ~2,250,000. */
/* one encoded access unit is at most VbufSz (0x54000) and rides with the SPS/PPS header, so a stage buffer
 * is 0x60000. They were 0x220000 each for tap's uncompressed USB frames; the USB path is gone. */
//  controlled baseline: restore the pre-trim stage/arena layout.
// This is not evidence that 's smaller layout caused the regression.
constexpr size_t StreamStageSize = 0x220000; /* per stream stage buffer */
static_assert(0x360000 + 8847360 + 3 * StreamStageSize <= 24u * 1024 * 1024,
              "vic buffers + slot + stage buffers must fit the heap");
constinit size_t g_vic_heap_size = 0;

/* The capture buffer stays on the HEAP, deliberately.
 *
 * Moving it to a 4 MB .bss array to dodge the 2 MB heap cap fataled a
 * different sysmodule (0100000000000023) with 0x10801 LimitReached at
 * boot: a sysmodule's static memory is drawn from the shared system
 * pool (pool_partition 2), so taking 4 MB of it starves everyone else.
 * This project already learned that once, when the early recon module
 * carried 9.3 MB of .bss.
 *
 * Heap is the safe place, so we live within the 2 MB ceiling and let
 * the resolution picker choose whatever actually fits. */
constinit size_t g_ind_size = 0;

constinit uintptr_t g_vic_heap = 0;
constinit bool g_engine_wedged = false; /* up here - CaptureRelease checks it */
constinit u8 *g_vic_cfg_buf = nullptr;
constinit u8 *g_vic_cmd_buf = nullptr;
constinit u8 *g_vic_dst_buf = nullptr;
constinit u8 *g_vic_src_buf = nullptr;
constinit u8 *g_ind_buf = nullptr;

// Preserve the existing capture allocation while removing the unused upstream staging buffers.
constexpr size_t CaptureTailBytes = 224 * 1024;

/* cached heap past the nvmap'd block-row, used to stage uncached engine
 * output before it goes to the filesystem */
constinit u8 *g_stage_buf = nullptr;
/* two stage buffers so the USB transfer of frame N overlaps the
 * capture of frame N+1. Each holds [32-byte header][frame], posted as a
 * single URB - one transfer per frame, not two. */
constinit u8 *g_stream_stage[2] = {nullptr, nullptr};

/* usbDs requires a 0x1000-aligned buffer, so the 32-byte frame
 * header cannot be posted from the stack. */
alignas(0x1000) constinit u8 g_usb_hdr[0x1000] = {};
/* queued game audio, sent as its own packet before each frame */

/* the decisive measurement. LogMemoryPools in applet_mitm_main.cpp
 * is in an anonymous namespace, so it cannot be reached from here - this
 * is the same survey, taken either side of the heap grab.
 *
 * pool_partition moved 2 -> 1, so the bytes must now come out of Applet
 * (511,936 KB free at boot) and NOT out of System (15,452 KB), which is
 * the pool am was starved of when  fataled the console. Measuring
 * both sides is the point: the kernel chain says Applet, but 's
 * reading was reasoning that turned out to be wrong, and only the
 * before/after delta shows which pool actually drained. */

/* the VIC heap and capture region come from the memory donor
 * (applet_mitm_donor.hpp), not our own heap */
constinit bool g_donor_mode = false;

void SetVicLayout(uintptr_t addr, size_t size)
{
    g_vic_heap_size = size;
    g_vic_heap = addr;
    g_vic_cfg_buf = reinterpret_cast<u8 *>(addr + 0x0000);
    g_vic_cmd_buf = reinterpret_cast<u8 *>(addr + 0x4000);
    g_vic_dst_buf = reinterpret_cast<u8 *>(addr + 0x10000);
    g_vic_src_buf = reinterpret_cast<u8 *>(addr + 0x350000);
    g_ind_buf = nullptr;

    g_ind_size = 0;
    g_stage_buf = nullptr;
    g_stream_stage[0] = g_stream_stage[1] = nullptr;
    std::memset(reinterpret_cast<void *>(addr), 0, VicBaseHeap);
}

bool AllocVicHeap()
{
    if (g_vic_heap != 0)
    {
        return true;
    }

    {
        uintptr_t daddr = 0;
        size_t dsize = 0;
        if (DonorObtain(std::addressof(daddr), std::addressof(dsize)))
        {
            if (dsize >= VicBaseHeap + 6_MB)
            {
                g_donor_mode = true;
                SetVicLayout(daddr, VicBaseHeap);
                g_vic_heap_size = dsize;
                LogLine("   VIC heap from the donor: %zu MB at 0x%lx (engine buffers %zu MB, capture region %zu MB)",
                        dsize >> 20, static_cast<unsigned long>(daddr), VicBaseHeap >> 20, (dsize - VicBaseHeap) >> 20);
                return true;
            }
            LogLine("   donor gave only %zu MB", dsize >> 20);
            DonorRelease();
        }
        else
        {
            LogLine("   no donor");
        }
        /* the module lives in the System pool, where every
         * custom sysmodule shares a few MB - never take the streaming
         * memory from there */
        LogLine("   not streaming: no memory from the donor (is atmosphere/contents/0100000000000C21 installed?)");
        return false;

    }
}

/* nvmap handles on the capture region not yet freed (NvfSession
 * creates two per session: the slot copy and the arena) - the region
 * is never given back while any is alive */
constinit u32 g_capture_handles = 0;

/* the capture region, right after the engine buffers. The
 * ladder is the old one less the 4 MB base: 1080p pipelining wants the
 * arena up to ~31.5 MB; a refusal just descends (720p needs ~22 MB). */
bool CaptureAcquire(const char *who)
{
    if (g_ind_buf != nullptr)
    {
        return true;
    }
    if (g_vic_heap == 0)
    {
        return false;
    }
    if (g_donor_mode)
    {
        const size_t cap = g_vic_heap_size - VicBaseHeap - CaptureTailBytes;
        g_ind_buf = reinterpret_cast<u8 *>(g_vic_heap + VicBaseHeap);
        g_ind_size = cap;

        g_stage_buf = g_ind_buf + FbBlockRowStage;
        g_stream_stage[0] = g_stage_buf + StreamStageSize;
        g_stream_stage[1] = g_stream_stage[0] + StreamStageSize;
        std::memset(g_ind_buf, 0, cap);
        LogLine("   %s: capture memory (donor): %zu MB at %p", who, cap / (1024 * 1024),
                static_cast<void *>(g_ind_buf));
        return true;
    }
    size_t total = 0;
    /* every rung 2 MB up with the base heap, so the capture
     * region is exactly the size it was (frame rate and latency unchanged) */
    for (const size_t sz : {42_MB, 38_MB, 34_MB, 30_MB, 26_MB, 22_MB, 18_MB, 14_MB, 12_MB})
    {
        if (R_SUCCEEDED(os::SetMemoryHeapSize(sz)))
        {
            total = sz;
            break;
        }
    }
    if (total == 0)
    {
        LogLine("   %s: capture memory: no heap size accepted - the Applet pool is short (an applet open?)", who);
        return false;
    }
    const size_t block = total - VicBaseHeap;
    const size_t cap = block - CaptureTailBytes;
    uintptr_t addr = 0;
    if (const auto rc = os::AllocateMemoryBlock(std::addressof(addr), block); R_FAILED(rc))
    {
        LogLine("   %s: capture memory: AllocateMemoryBlock(%zu MB) rc=0x%x", who, cap / (1024 * 1024), rc.GetValue());
        static_cast<void>(os::SetMemoryHeapSize(VicBaseHeap));
        return false;
    }
    g_ind_buf = reinterpret_cast<u8 *>(addr);
    g_ind_size = cap;

    g_stage_buf = g_ind_buf + FbBlockRowStage;
    g_stream_stage[0] = g_stage_buf + StreamStageSize;
    g_stream_stage[1] = g_stream_stage[0] + StreamStageSize;
    g_vic_heap_size = total;
    std::memset(g_ind_buf, 0, cap);
    LogLine("   %s: capture memory taken: %zu MB at %p (heap %zu MB)", who, cap / (1024 * 1024),
            static_cast<void *>(g_ind_buf), total / (1024 * 1024));
    return true;
}

/* the capture region back to the system - only with no nvmap handle
 * on it and no engine job that may still write it */
void CaptureRelease(const char *who)
{
    if (g_ind_buf == nullptr)
    {
        return;
    }
    static bool said_kept = false;
    if (g_engine_wedged || g_capture_handles != 0)
    {
        if (!said_kept)
        {
            LogLine("   %s: capture memory kept (%s)", who,
                    g_engine_wedged ? "an engine job never completed" : "an nvmap handle on it is still alive");
            said_kept = true;
        }
        return;
    }
    if (g_donor_mode)
    {
        /* the donor's memory stays mapped */
        g_ind_buf = nullptr;

        g_ind_size = 0;
        g_stage_buf = nullptr;
        g_stream_stage[0] = g_stream_stage[1] = nullptr;
        LogLine("   %s: capture memory (donor) released", who);
        return;
    }
    os::FreeMemoryBlock(reinterpret_cast<uintptr_t>(g_ind_buf), g_ind_size + CaptureTailBytes);
    const auto rc = os::SetMemoryHeapSize(VicBaseHeap);
    LogLine("   %s: capture memory given back (%zu MB), heap back to %zu MB rc=0x%x", who, g_ind_size / (1024 * 1024),
            VicBaseHeap / (1024 * 1024), rc.GetValue());
    g_ind_buf = nullptr;

    g_ind_size = 0;
    g_stage_buf = nullptr;
    g_stream_stage[0] = g_stream_stage[1] = nullptr;
    g_vic_heap_size = VicBaseHeap;
}

/* Phase B geometry: a scale-free 64x64 crop of the frame's top-left.
 * The output STRIDE is aligned to 256 pixels, not to the width: that is
 * what libdrm's vic_image_new does for every VIC surface, pitch-linear
 * included (align = 256, stride = ALIGN(width, align)). 64 px of stride
 * would be a 256-byte pitch, well under what the engine expects. */
constexpr u32 DstW = 64;
constexpr u32 DstH = 64;
constexpr u32 DstStridePx = 256; /* 1024 B pitch */
constexpr u32 DstPitch = DstStridePx * 4;
constexpr u32 DstSize = DstPitch * DstH; /* 65536 */
static_assert(DstSize <= VicDstSize);

constinit ::Service g_nv_srv = {};
constinit os::NativeHandle g_nv_tmem_handle = os::InvalidNativeHandle;
constinit u64 g_nv_tmem_size = 0;

/* Linux-style ioctl encoding: (dir<<30)|(size<<16)|(type<<8)|nr, IOWR dir=3 */
constexpr u32 MakeIowr(u32 type, u32 nr, u32 size)
{
    return (UINT32_C(3) << 30) | (size << 16) | (type << 8) | nr;
}
constexpr u32 MakeIow(u32 type, u32 nr, u32 size)
{
    return (UINT32_C(1) << 30) | (size << 16) | (type << 8) | nr;
}
constexpr u32 NvmapIocCreate = MakeIowr(0x01, 0x01, 8);
constexpr u32 NvmapIocAlloc = MakeIowr(0x01, 0x04, 32);
constexpr u32 NvmapIocGetId = MakeIowr(0x01, 0x0E, 8);
constexpr u32 NvmapIocFree = MakeIowr(0x01, 0x05, 24); /* libnx nvioctlNvmap_Free */

constexpr u32 NvHostIocChannelGetSyncpoint = MakeIowr(0x00, 0x02, 8); /* {u32 module_id; u32 syncpt}   */
constexpr u32 NvHostIocChannelSetSubmitTo = MakeIow(0x00, 0x07, 4);   /* {u32 timeout}                 */
constexpr u32 NvHostIocChannelSetNvmapFd = MakeIow(0x48, 0x01, 4);    /* {u32 fd}  -> 0x40044801       */
/* MAP_CMD_BUFFER pins nvmap handles and RETURNS their device physical
 * address. Horizon replaced Linux's per-submit pinning with this, which
 * is why a submit carrying relocs for unpinned handles answers
 * InvalidState(8). Header is 12 B (num_handles, reserved, is_compr,
 * pad[3]) + 8 B per handle -> 20 for one. */
constexpr u32 NvHostIocChannelMapCmdBuf = MakeIowr(0x00, 0x09, 20); /* -> 0xC0140009 */
constexpr u32 NvHostIocChannelUnmapCmdBuf = MakeIowr(0x00, 0x0A, 20);
constexpr u32 NvHostIocChannelMapCmdBufEx = MakeIowr(0x00, 0x25, 20); /* unpins on error */
constexpr u32 NvHostIocCtrlSyncptRead = MakeIowr(0x00, 0x14, 8);      /* {u32 id; u32 value}           */
constexpr u32 NvHostIocCtrlSyncptWait = MakeIowr(0x00, 0x16, 12);     /* {u32 id; u32 thresh; u32 to}  */
constexpr u32 NvHostIocCtrlSyncptIncrW = MakeIow(0x00, 0x15, 4);      /* {u32 id} - switchbrew 0x40040015 */
constexpr u32 NvHostIocCtrlSyncptIncrWR = MakeIowr(0x00, 0x15, 4);    /* libnx encodes this one instead   */

struct MapCmdBufArgs
{
    u32 num_handles;
    u32 reserved;
    u8 is_compr;
    u8 padding[3];
    u32 handle_id_in;
    u32 phys_addr_out;
};
static_assert(sizeof(MapCmdBufArgs) == 20);
static_assert(__builtin_offsetof(MapCmdBufArgs, handle_id_in) == 12);

/* breadcrumb + stage, so the heartbeat can name where a hang happened */
/* M64b: LogMark does WriteWhole(applet-mitm.last) AND LogLine - two SD
 * writes per call. RunOneJob calls this on entry, so the stage
 * breadcrumb alone cost ~41 ms/job: the bench measured 0.83 ms of
 * engine time inside a 39,418 us wall clock. The atomic store is free
 * and the heartbeat still reports it, so quiet mode keeps the store and
 * drops the write. */
/* declared here because VicStage below is the first user */
constinit bool g_vic_quiet = false;

void VicStage(const char *s)
{
    if (!g_vic_quiet)
    {
        LogMark(s);
    }
}

::Result NvIoctl(u32 fd, u32 request, void *argp, size_t argsz, u32 *out_err)
{
    const struct
    {
        u32 fd;
        u32 request;
    } in = {fd, request};
    u32 error = 0;
    const ::Result rc = serviceDispatchInOut(std::addressof(g_nv_srv), 1, in, error,
                                             .buffer_attrs =
                                                 {
                                                     SfBufferAttr_HipcAutoSelect | SfBufferAttr_In,
                                                     SfBufferAttr_HipcAutoSelect | SfBufferAttr_Out,
                                                 },
                                             .buffers = {{argp, argsz}, {argp, argsz}}, );
    *out_err = error;
    return rc;
}

::Result NvOpen(const char *path, u32 *out_fd, u32 *out_err)
{
    struct
    {
        u32 fd;
        u32 error;
    } out = {};
    const ::Result rc = serviceDispatchOut(std::addressof(g_nv_srv), 0, out,
                                           .buffer_attrs = {SfBufferAttr_In | SfBufferAttr_HipcMapAlias},
                                           .buffers = {{path, std::strlen(path)}});
    *out_fd = out.fd;
    *out_err = out.error;
    return rc;
}

/* nvdrv cmd 7. Plain u64 in, u32 err out - no PID descriptor, so unlike
 * vi's OpenLayer this one is actually expressible for us. Needs
 * NvDrvPermission bit 10, which only nvdrv:t grants.
 *
 * libnx's nvInitialize does the equivalent for every normal client:
 *     u64 aruid = appletGetAppletResourceUserId();
 *     if (aruid) _nvSetClientPID(aruid);
 * Our session has never set one, so it is aruid 0 while the game's
 * buffers belong to the game's aruid. That asymmetry is the last
 * candidate explanation for the silent phys=0. */

void NvClose(u32 fd)
{
    const struct
    {
        u32 fd;
    } in = {fd};
    u32 e = 0;
    serviceDispatchInOut(std::addressof(g_nv_srv), 2, in, e);
}

/* Pin one nvmap handle into the channel and get its device physical
 * address. Each call is breadcrumbed by the caller so that if this ever
 * takes nvservices down again we know exactly which handle did it. */
::Result NvmapOwn(u32 fd, void *cpu, u32 size, u8 kind, u32 *out_handle, u32 *out_id, bool cacheable = false);

/* device address 0 is a real address the channels hand out
 * when it is free, and to every check here 0 means "not pinned" (a zero
 * address hangs the VIC). The guard page mapped at setup only takes it
 * if it is free then; a later session found it freed and got it for its
 * slot buffer ("slot pin returned 0"). So whenever a mapping comes back
 * at 0: unmap it, map a fresh guard page from the unused gap in our
 * heap (it lands at 0 and stays there, never unmapped), map again. */
constinit u32 g_guard_nvmap_fd = 0;
constinit u32 g_guard_next = 0x9000; /* 0x9000..0xF000: the gap between cmd and dst */

bool MapCmdBuffer(u32 chan_fd, u32 handle, u32 *out_addr, const char *what, u8 is_compr = 0, u32 req = 0);

bool AbsorbDeviceZero(u32 chan_fd, u32 req)
{
    if (g_guard_nvmap_fd == 0 || g_vic_heap == 0 || g_guard_next >= 0x10000)
    {
        return false;
    }
    u32 gh = 0, gid = 0;
    if (R_FAILED(NvmapOwn(g_guard_nvmap_fd, reinterpret_cast<u8 *>(g_vic_heap + g_guard_next), 0x1000, 0,
                          std::addressof(gh), std::addressof(gid), false)))
    {
        return false;
    }
    g_guard_next += 0x1000;
    MapCmdBufArgs a = {};
    a.num_handles = 1;
    a.handle_id_in = gh;
    u32 nverr = 0;
    const ::Result rc = NvIoctl(chan_fd, req, std::addressof(a), sizeof(a), std::addressof(nverr));
    LogLine("   guard page mapped at 0x%x (rc=0x%x nverr=%u) - device address 0 is held from now on", a.phys_addr_out,
            rc, nverr);
    return R_SUCCEEDED(rc) && nverr == 0 && a.phys_addr_out == 0;
}

bool MapCmdBuffer(u32 chan_fd, u32 handle, u32 *out_addr, const char *what, u8 is_compr, u32 req)
{
    MapCmdBufArgs a = {};
    a.num_handles = 1;
    a.is_compr = is_compr;
    a.handle_id_in = handle;
    u32 nverr = 0;
    if (req == 0)
    {
        req = NvHostIocChannelMapCmdBuf;
    }
    const ::Result rc = NvIoctl(chan_fd, req, std::addressof(a), sizeof(a), std::addressof(nverr));
    LogLine("   MAP_CMD_BUFFER(%s handle=%u compr=%u req=0x%08x) rc=0x%x nverr=%u -> phys=0x%x", what, handle, is_compr,
            req, rc, nverr, a.phys_addr_out);
    if (R_FAILED(rc) || nverr != 0)
    {
        return false;
    }
    if (a.phys_addr_out == 0 && AbsorbDeviceZero(chan_fd, req))
    {
        /* ours was at 0: move it off */
        MapCmdBufArgs u = {};
        u.num_handles = 1;
        u.handle_id_in = handle;
        u32 ue = 0;
        NvIoctl(chan_fd, NvHostIocChannelUnmapCmdBuf, std::addressof(u), sizeof(u), std::addressof(ue));
        a = {};
        a.num_handles = 1;
        a.is_compr = is_compr;
        a.handle_id_in = handle;
        nverr = 0;
        const ::Result rc2 = NvIoctl(chan_fd, req, std::addressof(a), sizeof(a), std::addressof(nverr));
        LogLine("   MAP_CMD_BUFFER(%s) again -> phys=0x%x (rc=0x%x nverr=%u)", what, a.phys_addr_out, rc2, nverr);
        if (R_FAILED(rc2) || nverr != 0)
        {
            return false;
        }
    }
    *out_addr = a.phys_addr_out;
    return true;
}

void UnmapCmdBuffer(u32 chan_fd, u32 handle)
{
    MapCmdBufArgs a = {};
    a.num_handles = 1;
    a.handle_id_in = handle;
    u32 nverr = 0;
    NvIoctl(chan_fd, NvHostIocChannelUnmapCmdBuf, std::addressof(a), sizeof(a), std::addressof(nverr));
}

bool NvmapFree(u32 fd, u32 handle);

/* .1: before a session pins its buffers, a guard page first. A
 * hole at device address 0 opens when another process's buffers that
 * held it go away (a homebrew app closing): the guard fills it and
 * stays; with 0 held already it is not needed and goes again. Without
 * it the slot copy could land on 0, which the VIC hangs on - a black
 * picture until a reboot. (Moving our own mapping off 0 afterwards
 * does not work: an unmapped handle keeps its address until freed.) */
void HoldDeviceZero(u32 chan_fd)
{
    if (g_guard_nvmap_fd == 0 || g_vic_heap == 0 || g_guard_next >= 0x10000)
    {
        return;
    }
    u32 gh = 0, gid = 0;
    if (R_FAILED(NvmapOwn(g_guard_nvmap_fd, reinterpret_cast<u8 *>(g_vic_heap + g_guard_next), 0x1000, 0,
                          std::addressof(gh), std::addressof(gid), false)))
    {
        return;
    }
    MapCmdBufArgs a = {};
    a.num_handles = 1;
    a.handle_id_in = gh;
    u32 nverr = 0;
    const ::Result rc =
        NvIoctl(chan_fd, NvHostIocChannelMapCmdBuf, std::addressof(a), sizeof(a), std::addressof(nverr));
    if (R_SUCCEEDED(rc) && nverr == 0 && a.phys_addr_out == 0)
    {
        g_guard_next += 0x1000;
        LogLine("   device address 0 was free - a guard page holds it now");
        return;
    }
    if (R_SUCCEEDED(rc) && nverr == 0)
    {
        UnmapCmdBuffer(chan_fd, gh);
    }
    static_cast<void>(NvmapFree(g_guard_nvmap_fd, gh));
}

/* CREATE + ALLOC(kind, our cpu pages) + GET_ID for a buffer we own. */
::Result NvmapOwn(u32 fd, void *cpu, u32 size, u8 kind, u32 *out_handle, u32 *out_id, bool cacheable)
{
    u32 nverr = 0;
    struct
    {
        u32 size;
        u32 handle;
    } cr = {size, 0};
    ::Result rc = NvIoctl(fd, NvmapIocCreate, std::addressof(cr), sizeof(cr), std::addressof(nverr));
    if (R_FAILED(rc) || nverr != 0)
    {
        LogLine("   nvmapOwn CREATE fail rc=0x%x nverr=%u", rc, nverr);
        return rc;
    }

    struct
    {
        u32 handle;
        u32 heapmask;
        u32 flags;
        u32 align;
        u8 kind;
        u8 pad[7];
        u64 addr;
    } al = {};
    al.handle = cr.handle;
    al.flags = cacheable ? 1u : 0u; /* bit0 = CACHEABLE */
    al.align = 0x1000;
    al.kind = kind;
    al.addr = reinterpret_cast<u64>(cpu);
    rc = NvIoctl(fd, NvmapIocAlloc, std::addressof(al), sizeof(al), std::addressof(nverr));
    if (R_FAILED(rc) || nverr != 0)
    {
        LogLine("   nvmapOwn ALLOC fail rc=0x%x nverr=%u", rc, nverr);
        return rc;
    }

    /* flags bit0 is CACHEABLE (libnx passes is_cpu_cacheable?1:0), not
     * read-write as the wiki's comment suggests. For a non-cacheable
     * buffer we owe the matching CPU-side step that libnx does and we
     * never did: flush, then mark the mapping uncached. Without it the
     * CPU keeps a cached, non-coherent view.
     *
     * The capture buffer is the exception. It is written by
     * ReadDebugProcessMemory - a kernel memcpy of 983,040 B, every
     * frame - and an uncached destination would cost far more than
     * cache maintenance does. It stays CACHED, and the caller flushes
     * it before the engine reads. */
    armDCacheFlush(cpu, size);
    if (!cacheable)
    {
        const auto sma = svc::SetMemoryAttribute(reinterpret_cast<uintptr_t>(cpu), size, svc::MemoryAttribute_Uncached,
                                                 svc::MemoryAttribute_Uncached);
        LogLine("   SetMemoryAttribute(%p, 0x%x, uncached) rc=0x%x", cpu, size, sma.GetValue());
    }
    else
    {
        LogLine("   nvmapOwn(%p, 0x%x) kept CACHEABLE - caller must flush before submit", cpu, size);
    }

    struct
    {
        u32 id;
        u32 handle;
    } gi = {0, cr.handle};
    rc = NvIoctl(fd, NvmapIocGetId, std::addressof(gi), sizeof(gi), std::addressof(nverr));
    *out_handle = cr.handle;
    *out_id = gi.id;
    LogLine("   nvmapOwn ok handle=%u id=%u kind=%u size=0x%x", cr.handle, gi.id, kind, size);
    return rc;
}

/* free one of our nvmap handles. Until now none was ever freed
 * (two per stream session); the capture region can only go back to
 * the system once nothing references it. True when nvmap let go of the
 * memory (flags bit 0, NOT_FREED_YET, clear). */
/* The reply (yuzu's nvmap IocFree) is the freed memory's address and
 * size, and flags bit 0 = it was mapped uncached. Address and size 0:
 * our handle is gone but another reference (an internal dup) still
 * holds the memory. : test12 read flags bit 0 as "still
 * referenced", so every free counted as failed and the capture region
 * (and the donor's memory) was never given back. */
bool NvmapFree(u32 fd, u32 handle)
{
    struct
    {
        u32 handle;
        u32 pad;
        u64 address;
        u32 size;
        u32 flags;
    } a = {handle, 0, 0, 0, 0};
    static_assert(sizeof(a) == 24);
    u32 nverr = 0;
    const ::Result rc = NvIoctl(fd, NvmapIocFree, std::addressof(a), sizeof(a), std::addressof(nverr));
    if (R_FAILED(rc) || nverr != 0)
    {
        LogLine("   nvmap FREE(handle %u) rc=0x%x nverr=%u - failed", handle, rc, nverr);
        return false;
    }
    if (a.address == 0 && a.size == 0)
    {
        LogLine("   nvmap FREE(handle %u): our handle is gone, the memory is still referenced elsewhere", handle);
        return false;
    }
    return true;
}

// NVENC and VIC share this guarded mapping arena.
enum class VicJob
{
    Fill,
    BlitSelf,
    BlitGame,
    BlitStrip
};

/* stride_px is what goes into SlotLumaWidth (+1). For pitch-linear that
 * is plainly the pixel stride. For BLOCK-linear nobody has told us
 * whether the field wants pixels or BYTES: libdrm only ever blits
 * pitch surfaces and marks both SlotBlkHeight and SlotCacheWidth
 * "XXX". So it is swept below rather than guessed. */
/* w/h describe the SURFACE; rect_w/rect_h the region actually sampled.
 * They differ only for the unscaled 1:1 control. */
struct SrcDesc
{
    u32 w, h, stride_px, blk_kind, blk_h_log2, pixfmt, cache_w, rect_w, rect_h;
    u32 rect_x = 0, rect_y = 0; /* the sampled region's top-left corner */
};

/* our own pitch-linear 64x64 buffer, stride 256 px like the destination */
constexpr SrcDesc SelfSrc{DstW, DstH, DstStridePx, vic::BLK_KIND_PITCH, 0, vic::PIXFMT_A8B8G8R8, vic::CACHE_WIDTH_64Bx4,
                          DstW, DstH};

/* The OUTPUT format has been hardcoded A8B8G8R8 since  and never
 * swept. The 64x64 self-blit could not reveal an R<->B swap either: its
 * painted ramp differed in R and B only by a constant. */
/* block-linear output, for NVENC's input. NVENC reads GPU
 * block-linear only (its surface config has no pitch mode), with
 * grc's block height 2 = 32-row blocks. Pitch stays the default. */
struct OutDesc
{
    u32 w, h, stride_px, fmt;
    u32 blk_kind = vic::BLK_KIND_PITCH;
    u32 blk_h_log2 = 0;
};

/* the 64x64 self-blit target, unchanged: it is the byte-exact regression
 * reference and must keep producing identical output */
constexpr OutDesc SelfOut{DstW, DstH, DstStridePx, vic::PIXFMT_A8B8G8R8};

/* One block-row of the GAME's surface: full 1920 px width by 128 rows,
 * block-linear, exactly what the binder parcels describe.
 *
 * NOTE the kind. SlotBlkKind is a 4-BIT field carrying the VIC's own
 * enum - 0 pitch, 1 generic 16Bx2 - not the nvmap kind 0xFE that the
 * parcels report. Writing 0xFE there truncates to 0xE and hands the
 * engine a surface layout that does not exist, and a bad layout hangs
 * the VIC, which takes the compositor and the console with it. */
constexpr u32 StripW = 1920, StripH = 128;
constexpr SrcDesc StripSrc{
    StripW, StripH, StripW, vic::BLK_KIND_GENERIC_16Bx2, 4, vic::PIXFMT_A8B8G8R8, vic::CACHE_WIDTH_64Bx4,
    StripW, StripH};
static_assert(StripSrc.rect_w > 0 && StripSrc.rect_h > 0, "a zero rect underflows to 0xFFFFFFFF in FillBlitConfig");
/* set per variant before each BlitStrip submit */
constinit SrcDesc g_strip_src = StripSrc;

/* 4x downscale into the existing 64 KB destination. The stride is
 * aligned to 256 px the way libdrm aligns every VIC surface :
 * 512 px = 2048 B, x 32 rows = 65536 B, exactly DstSize. */
/*  produced a completed blit whose pixels were scrambled: the right
 * brightness structure, no coherent image. That is a source-LAYOUT
 * error, not scale or crop. Three fields could cause it and none has a
 * reference, so sweep them in one boot the way  settled SETCL. */
/*  settled it, and it was never the layout. The engine's output came
 * back as [0xFF, G_src, B_src, A_src] - the source shifted down one lane
 * with alpha forced to max by ConstantAlpha/PlanarAlpha. Per-lane error
 * against a software de-swizzle of the same bytes:
 *
 *     aligned:  out[1]vsR 7.15   out[2]vsG 21.02   out[3]vsB 195.71
 *     shifted:  out[1]vsG 1.87   out[2]vsB  1.69   out[3]vsA   0.70
 *
 * Under 2 LSB on every lane once shifted - filter-difference magnitude.
 * So geometry, scale and block-linear addressing were all correct, and
 * the engine simply ate the source's R byte as alpha.
 *
 * The game's surface is R,G,B,A in memory, which the VIC calls
 * R8G8B8A8, not the A8B8G8R8 I declared. Sweep the three candidates
 * rather than assume, keeping the layout that already works. */
/* 's mapping table showed every format is a LOSSLESS permutation -
 * each output lane matched some source lane under 5 LSB, alpha included.
 * Nothing is destroyed; the channels are merely shuffled. P32_argb came
 * closest, with only R and B transposed:
 *
 *     out0(A)=srcA 1.11   out1(R)=srcB 4.12
 *     out2(G)=srcG 4.52   out3(B)=srcR 4.98
 *
 * So this sweeps SOURCE x OUTPUT format - the output side has been fixed
 * at A8B8G8R8 since  and never tested - and one of the four must be
 * the identity mapping.
 *
 * ONE2ONE is the separate question: the residual 4-5 LSB is either the
 * VIC's polyphase scaler differing from a box filter, or a small
 * sampling offset. An unscaled 480x32 crop cannot have a filter error,
 * so if it lands near zero the residual is filtering and harmless. */
/*  settled the VIC completely. ONE2ONE - the unscaled 480x32 crop -
 * matched a software de-swizzle with err 0.00 on ALL FOUR LANES. Not
 * approximately: bit for bit. So block-linear addressing, GOB tiling,
 * stride and block height are exact, and the 2-3 LSB seen on scaled
 * variants is just the VIC's polyphase scaler differing from a box
 * filter - not an error.
 *
 * Channel order: S32O33 maps out=ABGR, a pure R<->B swap from identity.
 * Every variant is a LOSSLESS permutation, so this costs nothing to undo
 * on the PC, or by choosing NVENC's input format - which has to be
 * configured anyway. Not worth another docked run.
 *
 * Two variants kept as regression only. */

constexpr OutDesc StripOut{480, 32, 512, vic::PIXFMT_A8B8G8R8};
constinit OutDesc g_strip_out = StripOut; /* set per variant */
/* the present's flips, applied on the VIC's output - what the
 * compositor does for a game that renders upside down */
constinit u32 g_strip_flip = 0;
constexpr u32 StripOutSize = StripOut.stride_px * 4 * StripOut.h;
static_assert(StripOutSize <= 0x60000, "the strip probe copies one strip into a stage buffer (StreamStageSize)");
static_assert(StripOutSize <= DstSize);

void FillOutputConfig(vic::VicConfigStruct *c, const OutDesc &out)
{
    c->outputConfig.TargetRectLeft = 0;
    c->outputConfig.TargetRectTop = 0;
    c->outputConfig.TargetRectRight = out.w - 1;
    c->outputConfig.TargetRectBottom = out.h - 1;

    c->outputSurfaceConfig.OutPixelFormat = out.fmt;
    c->outputSurfaceConfig.OutBlkKind = out.blk_kind;
    c->outputSurfaceConfig.OutBlkHeight = out.blk_h_log2;
    c->outputSurfaceConfig.OutSurfaceWidth = out.w - 1;
    c->outputSurfaceConfig.OutSurfaceHeight = out.h - 1;
    c->outputSurfaceConfig.OutLumaWidth = out.stride_px - 1;
    c->outputSurfaceConfig.OutLumaHeight = out.h - 1;
    /* 16383 is the "no chroma plane" sentinel the RGBA path has always
     * used. NV12 needs real dimensions: the UV plane is half resolution
     * in both axes, interleaved, so it is stride/2 x h/2 pairs. */
    if (vic::IsNv12(out.fmt))
    {
        c->outputSurfaceConfig.OutChromaWidth = (out.stride_px / 2) - 1;
        c->outputSurfaceConfig.OutChromaHeight = (out.h / 2) - 1;

        auto &m = c->outColorMatrixStruct;
        // Input conversion supplies the output colour space.
        m.matrix_enable = 0;

    }
    else
    {
        c->outColorMatrixStruct.matrix_enable = 0;
        c->outputSurfaceConfig.OutChromaWidth = 16383;
        c->outputSurfaceConfig.OutChromaHeight = 16383;
    }
}

/* Bytes of the luma plane, i.e. where the interleaved UV plane starts.
 * Must be 256-aligned because host1x carries addresses shifted right by
 * 8; every resolution we use satisfies that (1920x1080 = 256*8100).
 * a block-linear plane occupies whole blocks, so its height
 * rounds up to 8 << blk_h_log2 rows (720 -> 736 at grc's 32). */
constexpr u32 Nv12PlaneRows(u32 rows, const OutDesc &out)
{
    const u32 bh = (out.blk_kind == vic::BLK_KIND_PITCH) ? 1u : (8u << out.blk_h_log2);
    return (rows + bh - 1) / bh * bh;
}
constexpr u32 Nv12LumaBytes(const OutDesc &out)
{
    return out.stride_px * Nv12PlaneRows(out.h, out);
}
constexpr u32 Nv12ChromaBytes(const OutDesc &out)
{
    return out.stride_px * Nv12PlaneRows(out.h / 2, out);
}
static_assert(Nv12LumaBytes(OutDesc{1280, 720, 1280, vic::PIXFMT_Y8_U8V8_N420, vic::BLK_KIND_GENERIC_16Bx2, 2}) ==
              1280 * 736);
static_assert(Nv12ChromaBytes(OutDesc{1280, 720, 1280, vic::PIXFMT_Y8_U8V8_N420, vic::BLK_KIND_GENERIC_16Bx2, 2}) ==
              1280 * 384);
static_assert(Nv12LumaBytes(OutDesc{640, 360, 640, vic::PIXFMT_Y8_U8V8_N420}) == 640 * 360, "pitch output unchanged");

/* libdrm vic40_fill / vic_clear: paint the whole target one colour with
 * no slot enabled. Anything non-zero in dst afterwards proves the whole
 * output half of the pipeline. */
void FillClearConfig(vic::VicConfigStruct *c)
{
    std::memset(c, 0, sizeof(*c));
    FillOutputConfig(c, SelfOut);
    /* Four DISTINCT levels so the output byte order can be read straight
     * off the dump: 1023->0xFF, 768->0xC0, 512->0x80, 256->0x40.
     * The previous red gave 'ff ff 00 00', which could not say which
     * byte was alpha and which was red. */
    c->outputConfig.BackgroundAlpha = 1023; /* 0xFF */
    c->outputConfig.BackgroundR = 768;      /* 0xC0 */
    c->outputConfig.BackgroundG = 512;      /* 0x80 */
    c->outputConfig.BackgroundB = 256;      /* 0x40 */
}

void FillBlitConfig(vic::VicConfigStruct *c, const SrcDesc &src, const OutDesc &out)
{
    std::memset(c, 0, sizeof(*c));

    FillOutputConfig(c, out);
    c->outputConfig.BackgroundAlpha = 1023;
    c->outputConfig.BackgroundR = 1023;
    c->outputConfig.BackgroundG = 1023;
    c->outputConfig.BackgroundB = 1023;

    /* slot 0: the sampled rect of the frame (the top-left DstW x DstH unless the game presents an offset window) */
    vic::SlotConfig *slot = std::addressof(c->slotStruct[0].slotConfig);
    slot->SlotEnable = 1;
    slot->CurrentFieldEnable = 1;
    slot->PlanarAlpha = 1023;
    slot->ConstantAlpha = 1;
    slot->SourceRectLeft = static_cast<u64>(src.rect_x) << 16; /* 16.16 fixed point */
    slot->SourceRectRight = static_cast<u64>(src.rect_x + src.rect_w - 1) << 16;
    slot->SourceRectTop = static_cast<u64>(src.rect_y) << 16;
    slot->SourceRectBottom = static_cast<u64>(src.rect_y + src.rect_h - 1) << 16;
    slot->DestRectLeft = 0;
    slot->DestRectRight = out.w - 1;
    slot->DestRectTop = 0;
    slot->DestRectBottom = out.h - 1;
    slot->SoftClampHigh = 1023;

    vic::SlotSurfaceConfig *s = std::addressof(c->slotStruct[0].slotSurfaceConfig);
    s->SlotPixelFormat = src.pixfmt;
    s->SlotBlkKind = src.blk_kind;
    s->SlotBlkHeight = src.blk_h_log2;
    s->SlotCacheWidth = src.cache_w;
    s->SlotSurfaceWidth = src.w - 1;
    s->SlotSurfaceHeight = src.h - 1;
    s->SlotLumaWidth = src.stride_px - 1;
    s->SlotLumaHeight = src.h - 1;
    s->SlotChromaWidth = 16383;
    s->SlotChromaHeight = 16383;
}

/* host1x pushbuf: VIC_PUSH_METHOD / VIC_PUSH_BUFFER from libdrm vic.h.
 * Each entry: [INCR(0x10,2)] [method>>2] [value|reloc-placeholder]. */
constexpr u32 Host1xIncr0x10x2 = (UINT32_C(1) << 28) | (0x10u << 16) | 2u;

/* Word indices of the three address slots, for the reloc variant. */
constexpr u32 CfgAddrWord = 8, DstAddrWord = 11, SrcAddrWord = 14; /* + 1 if SETCL is emitted */

u32 BuildCmdbuf(u32 *w, VicJob job, u32 cfg_addr, u32 dst_addr, u32 src_addr, bool set_class, u32 chroma_addr = 0)
{
    const bool has_src = (job == VicJob::BlitSelf || job == VicJob::BlitGame || job == VicJob::BlitStrip);
    u32 n = 0;
    /* Point the channel at the VIC's register space before touching
     * METHOD_OFFSET/METHOD_DATA, which are per-class registers. */
    if (set_class)
    {
        w[n++] = vic::Host1xOpcodeSetClass(0, vic::HOST1X_CLASS_VIC, 0);
    }
    w[n++] = Host1xIncr0x10x2;
    w[n++] = vic::SET_APPLICATION_ID >> 2;
    w[n++] = 1;
    w[n++] = Host1xIncr0x10x2;
    w[n++] = vic::SET_CONTROL_PARAMS >> 2;
    w[n++] = (static_cast<u32>(sizeof(vic::VicConfigStruct) / 16)) << 16;
    /* MAP_CMD_BUFFER already handed back a device address for buffers we
     * own, so those go in directly and the submit needs no reloc list.
     * The BlitReloc variant instead leaves all three as placeholders and
     * lets nvservices patch them. */
    w[n++] = Host1xIncr0x10x2;
    w[n++] = vic::SET_CONFIG_STRUCT_OFFSET >> 2;
    w[n++] = cfg_addr >> 8;
    w[n++] = Host1xIncr0x10x2;
    w[n++] = vic::SET_OUTPUT_SURFACE_LUMA_OFFSET >> 2;
    w[n++] = dst_addr >> 8;
    /* NV12 writes two planes, so the engine needs the UV base too. */
    if (chroma_addr != 0)
    {
        w[n++] = Host1xIncr0x10x2;
        w[n++] = vic::SET_OUTPUT_SURFACE_CHROMA_U_OFFSET >> 2;
        w[n++] = chroma_addr >> 8;
    }
    if (has_src)
    {
        w[n++] = Host1xIncr0x10x2;
        w[n++] = vic::SET_SURFACE0_SLOT0_LUMA_OFFSET >> 2;
        w[n++] = src_addr >> 8;
    }
    w[n++] = Host1xIncr0x10x2;
    w[n++] = vic::EXECUTE >> 2;
    w[n++] = 1u << 8;
    return n;
}

void RunLive(u32 vfd, u32 nvmap_fd, u32 cmd_handle, u32 syncpt, u32 cfg_addr);

/* why a stream ended - live mode decides what to wait for next */
enum class StreamEnd
{
    Done,
    GameGone,
    ViewerGone,
    EngineStall,
    Failed,
    Reconfigure,
    Disabled
};
/* attach, read one finished frame, write it as a PNG, detach */
StreamEnd TryNvencStream(::ams::svc::Handle dbg, u64 slot_base, u32 vfd, u32 nvmap_fd, u32 cmd_handle, u32 vsyncpt,
                         u32 cfg_addr, const GameSurface *geo = nullptr, bool composed = false);

// BT.709 conversion coefficients for the VIC RGB-to-NV12 blit.
struct CscProbe
{
    char name[16];
    u32 where;   /* 0 none, 1 output matrix, 2 slot-0 matrix */
    u32 shift;   /* matrix_r_shift */
    s32 c[3][4]; /* row i: c[i][0..2] on inputs 0..2, c[i][3] the offset column */
};
constinit const CscProbe *g_csc_override = nullptr;

/* set when an engine job never completed and its channel was left
 * open. Every later engine probe in this boot refuses to run. */

struct JobCtx
{
    u32 vfd, cmd_handle, syncpt;
    u32 cfg_addr, dst_addr, src_addr, src_off;
    u32 cfg_handle, dst_handle, src_handle;
    SrcDesc game_src;
    /* false when dst_addr is NOT g_vic_dst_buf (nvframe/nvstream
     * write into their own arena).  A "[nf:vic0] changed=0/65536
     * (untouched)" line was this check reading g_vic_dst_buf while the VIC
     * wrote the arena - the output was fine. */
    bool out_is_dst_buf = true;
};

// Encode from the VIC conversion buffers after its completion fence.
constinit u32 g_ctrl_fd = 0;

/* One open for the life of the process, instead of one per job. */
u32 CtrlFd()
{
    if (g_ctrl_fd == 0)
    {
        u32 fd = 0, e = 0;
        if (R_SUCCEEDED(NvOpen("/dev/nvhost-ctrl", std::addressof(fd), std::addressof(e))) && e == 0)
        {
            g_ctrl_fd = fd;
        }
    }
    return g_ctrl_fd;
}

/* Program the syncpoint increment the submit already promised. Omitting
 * this is what froze the console: nvhost raised syncpoint 12's max to
 * the fence, nothing ever incremented it, and nvnflinger - which
 * composites every frame on that same VIC syncpoint - waited forever. */
u32 AppendIncrSyncpt(u32 *w, u32 n, u32 syncpt, bool after_engine_op)
{
    const u32 cond = after_engine_op ? vic::INCR_SYNCPT_COND_OP_DONE : vic::INCR_SYNCPT_COND_IMMEDIATE;
    w[n++] = vic::Host1xOpcodeNonIncr(vic::UCLASS_INCR_SYNCPT, 1);
    w[n++] = (cond << vic::INCR_SYNCPT_COND_SHIFT) | syncpt;
    return n;
}

/* One VIC job end to end: config, cmdbuf, submit, wait, rescue, read
 * back. Called once per VicJob so a single reboot answers which half of
 * the pipeline works. */
/* Returns false if the engine did not complete, so the caller can stop.
 *
 * The guard below is the lesson from two console freezes: a VIC handed a
 * zero address does not fail politely, it HANGS, and a hung VIC takes the
 * compositor and the whole console with it. Every address the engine will
 * dereference is checked here, once, rather than trusted per call site. */
bool RunOneJob(const char *stage, VicJob job, bool set_class, const JobCtx &c)
{
    VicStage(stage);

    const bool needs_src = (job == VicJob::BlitSelf || job == VicJob::BlitGame || job == VicJob::BlitStrip);
    if (c.cfg_addr == 0 || c.dst_addr == 0 || (needs_src && c.src_addr == 0))
    {
        LogLine("   [%s] REFUSING to submit: cfg=0x%x dst=0x%x src=0x%x - a zero "
                "address hangs the VIC and freezes the console",
                stage, c.cfg_addr, c.dst_addr, c.src_addr);
        return false;
    }

    auto *cfg = reinterpret_cast<vic::VicConfigStruct *>(g_vic_cfg_buf);
    switch (job)
    {
    case VicJob::Fill:
        FillClearConfig(cfg);
        break;
    case VicJob::BlitSelf:
        FillBlitConfig(cfg, SelfSrc, SelfOut);
        break;
    case VicJob::BlitGame:
        FillBlitConfig(cfg, c.game_src, SelfOut);
        break;
    case VicJob::BlitStrip:
        FillBlitConfig(cfg, g_strip_src, g_strip_out);
        cfg->outputConfig.OutputFlipX = (g_strip_flip & 1) ? 1 : 0;
        cfg->outputConfig.OutputFlipY = (g_strip_flip & 2) ? 1 : 0;
        break;
    }
    if (g_csc_override != nullptr && g_csc_override->where != 0)
    {
        const CscProbe &p = *g_csc_override;
        auto s20 = [](s32 v) -> u64 { return static_cast<u64>(static_cast<u32>(v) & 0xFFFFFu); };
        cfg->outColorMatrixStruct.matrix_enable = 0;
        auto &m = (p.where == 2) ? cfg->slotStruct[0].colorMatrixStruct : cfg->outColorMatrixStruct;
        m.matrix_coeff00 = s20(p.c[0][0]);
        m.matrix_coeff01 = s20(p.c[0][1]);
        m.matrix_coeff02 = s20(p.c[0][2]);
        m.matrix_coeff03 = s20(p.c[0][3]);
        m.matrix_coeff10 = s20(p.c[1][0]);
        m.matrix_coeff11 = s20(p.c[1][1]);
        m.matrix_coeff12 = s20(p.c[1][2]);
        m.matrix_coeff13 = s20(p.c[1][3]);
        m.matrix_coeff20 = s20(p.c[2][0]);
        m.matrix_coeff21 = s20(p.c[2][1]);
        m.matrix_coeff22 = s20(p.c[2][2]);
        m.matrix_coeff23 = s20(p.c[2][3]);
        m.matrix_r_shift = p.shift;
        m.matrix_enable = 1;
    }

    /* pick the OutDesc this job will actually use, so the chroma base
     * matches the config struct we just filled */
    const OutDesc &job_out = (job == VicJob::BlitStrip) ? g_strip_out : SelfOut;
    const u32 chroma_addr = vic::IsNv12(job_out.fmt) ? (c.dst_addr + Nv12LumaBytes(job_out)) : 0u;

    auto *w = reinterpret_cast<u32 *>(g_vic_cmd_buf);
    u32 words = BuildCmdbuf(w, job, c.cfg_addr, c.dst_addr, c.src_addr + c.src_off, set_class, chroma_addr);
    const u32 rw = set_class ? 1u : 0u; /* reloc word indices shift when SETCL leads */
    words = AppendIncrSyncpt(w, words, c.syncpt, true);

    /* Prefill with a poison pattern rather than zero. "All zero" cannot
     * distinguish "engine wrote zeros" from "engine never touched our
     * memory"; surviving 0xAB proves the latter outright. */
    if (!g_vic_quiet && c.out_is_dst_buf)
    {
        std::memset(g_vic_dst_buf, 0xAB, DstSize);
    }
    armDCacheFlush(g_vic_dst_buf, DstSize);
    armDCacheFlush(g_vic_cfg_buf, VicCfgSize);
    armDCacheFlush(g_vic_cmd_buf, VicCmdSize);

    const u32 nr = 0u; /* relocs are inert on Horizon - always inline the addresses */

    alignas(8) u8 sb[16 + 12 + 3 * (16 + 4) + 20 + 4] = {};
    u32 off = 0;
    auto put = [&](u32 v) {
        std::memcpy(sb + off, std::addressof(v), 4);
        off += 4;
    };

    put(1);
    put(nr);
    put(1);
    put(1);
    put(c.cmd_handle);
    put(0);
    put(words);
    put(c.syncpt);
    put(1);
    put(0);
    put(0);
    put(0);
    const u32 fence_off = off;
    put(0);
    const u32 sz = off;

    const u32 req = (UINT32_C(3) << 30) | (sz << 16) | (0x00u << 8) | 0x01u;
    u32 nverr = 0, fence_val = 0;
    ::Result rc = NvIoctl(c.vfd, req, sb, sz, std::addressof(nverr));
    std::memcpy(std::addressof(fence_val), sb + fence_off, 4);
    if (!g_vic_quiet)
    {
        LogLine("   [%s] setcl=%d words=%u cmd[0..3]=%08x %08x %08x %08x", stage, static_cast<int>(set_class), words,
                w[0], w[1], w[2], w[3]);
        LogLine("   [%s] req=0x%08x sz=%u nr=%u rc=0x%x nverr=%u -> fence=%u", stage, req, sz, nr, rc, nverr,
                fence_val);
    }
    if (R_FAILED(rc) || nverr != 0)
    {
        LogLine("   [%s] SUBMIT REJECTED", stage);
        return false;
    }

    /* Read the cmdbuf back: nvservices patches reloc targets in place,
     * so these words now hold the addresses the engine would be given. */
    armDCacheFlush(g_vic_cmd_buf, VicCmdSize);
    if (!g_vic_quiet)
        LogLine("   [%s] resolved addrs: cfg=0x%08x dst=0x%08x src=0x%08x (<<8: 0x%x 0x%x 0x%x)", stage,
                w[CfgAddrWord + rw], w[DstAddrWord + rw], (job == VicJob::Fill) ? 0u : w[SrcAddrWord + rw],
                w[CfgAddrWord + rw] << 8, w[DstAddrWord + rw] << 8,
                (job == VicJob::Fill) ? 0u : (w[SrcAddrWord + rw] << 8));

    bool completed = false;
    const u32 cfd = CtrlFd(); /* opened once, never closed per job */
    if (cfd != 0)
    {
        struct
        {
            u32 id;
            u32 thresh;
            u32 timeout;
        } a = {c.syncpt, fence_val, 100};
        nverr = 0;
        rc = NvIoctl(cfd, NvHostIocCtrlSyncptWait, std::addressof(a), sizeof(a), std::addressof(nverr));
        struct
        {
            u32 id;
            u32 value;
        } r = {c.syncpt, 0};
        u32 e2 = 0;
        NvIoctl(cfd, NvHostIocCtrlSyncptRead, std::addressof(r), sizeof(r), std::addressof(e2));
        completed = (r.value >= fence_val);

        /* M63b: SYNCPT_WAIT returns before the threshold is met. The
         * loud path only ever "worked" because ~15 ms of SD-flushing log
         * calls sat between the submit and this read, giving the engine
         * time to finish - the bench made that obvious the moment the
         * logging went away (warm-up OK, then 1/60 and 0/60).
         *
         * Poll instead. This does not depend on the wait ioctl's
         * semantics, and the elapsed time it reports IS the engine time,
         * which is the number  never actually measured. */
        if (!completed)
        {
            /* 300 polls (~30-60 ms) were not enough when the
             * compositor shares the VIC - Minecraft in ReverseNX's docked
             * mode took up to 450 ms - and a miss ended the stream.
             * A stream waits up to ~0.5 s; the one-shot probe as before. */
            const u32 spins = g_vic_quiet ? 5000 : 300;
            for (u32 spin = 0; spin < spins && !completed; ++spin)
            {
                os::SleepThread(TimeSpan::FromMicroSeconds(100));
                r.value = 0;
                NvIoctl(cfd, NvHostIocCtrlSyncptRead, std::addressof(r), sizeof(r), std::addressof(e2));
                completed = (r.value >= fence_val);
            }

        }
        if (!g_vic_quiet)
        {
            LogLine("   [%s] WAIT nverr=%u  syncpt=%u (want >= %u)%s", stage, nverr, r.value, fence_val,
                    completed ? "  OP_DONE fired" : "  ENGINE DID NOT COMPLETE");
        }

        // Keep the cache and fence operations ordered before inspecting engine output.
        if (!g_vic_quiet && r.value < fence_val)
        {
            VicStage("vb:RESCUE_INCR");
            u32 ireq = NvHostIocCtrlSyncptIncrW;
            for (u32 k = 0, missing = fence_val - r.value; k < missing && k < 64; k++)
            {
                struct
                {
                    u32 id;
                } ai = {c.syncpt};
                u32 ae = 0;
                NvIoctl(cfd, ireq, std::addressof(ai), sizeof(ai), std::addressof(ae));
                if (k == 0 && ae != 0)
                {
                    ireq = NvHostIocCtrlSyncptIncrWR;
                    NvIoctl(cfd, ireq, std::addressof(ai), sizeof(ai), std::addressof(ae));
                }
            }
            if (!g_vic_quiet)
                LogLine("   [%s] rescued syncpt %u up to fence %u", stage, c.syncpt, fence_val);
        }
        /* cfd is process-wide now - closing it here is what cost a
         * device open/close on every single job */
    }

    /* The VIC wrote through the device side; our cache still holds the
     * zeros we just stored. Without this invalidate the read-back is
     * guaranteed to look empty no matter what the engine did. */
    armDCacheFlush(g_vic_dst_buf, DstSize);
    if (g_vic_quiet)
    {
        return completed;
    }
    if (!c.out_is_dst_buf)
    {
        LogLine("   [%s] output went to a caller-owned buffer at %#x - the caller checks and saves it", stage,
                c.dst_addr);
        return completed;
    }

    u32 sum = 0, changed = 0;
    for (u32 i = 0; i < DstSize; i++)
    {
        sum += g_vic_dst_buf[i];
        if (g_vic_dst_buf[i] != 0xAB)
        {
            changed++;
        }
    }
    char hex[3 * 32 + 1];
    int k = 0;
    for (u32 i = 0; i < 32; i++)
    {
        k += std::snprintf(hex + k, sizeof(hex) - k, "%02x ", g_vic_dst_buf[i]);
    }
    LogLine("   [%s] dst sum32=%u changed=%u/%u  %s", stage, sum, changed, DstSize,
            (changed > 0) ? "*** ENGINE WROTE OUR MEMORY ***" : "(untouched - poison 0xAB intact)");
    LogLine("   [%s] dst[0..32]: %s", stage, hex);
    const u8 *mid = g_vic_dst_buf + (DstH / 2) * DstPitch;
    LogLine("   [%s] row %u: %02x %02x %02x %02x %02x %02x %02x %02x", stage, DstH / 2, mid[0], mid[1], mid[2], mid[3],
            mid[4], mid[5], mid[6], mid[7]);
    return completed;
}

} // namespace

/* the dmnt:cht read bounce (applet_mitm_dmdbg.cpp): nullptr until the VIC
 * heap exists - reads only happen while streaming, after it is taken */
u8 *VicBounceBuffer()
{
    return g_vic_heap != 0 ? reinterpret_cast<u8 *>(g_vic_heap + VicBounceOff) : nullptr;
}
size_t VicBounceBufferSize()
{
    return VicBounceSize;
}

constinit u32 g_probe_delay_s = 120;

constinit std::atomic<u32> g_queue_count{0};
constinit std::atomic<s32> g_queue_slot{-1};
constinit std::atomic<u64> g_queue_tick{0};
constinit std::atomic<u32> g_queue_fence_n{0};
constinit std::atomic<u32> g_queue_transform{0};
constinit std::atomic<u32> g_queue_crop_wh{0};
constinit std::atomic<bool> g_queue_crop_origin0{true};
constinit std::atomic<u32> g_queue_crop_xy{0};
constinit std::atomic<bool> g_viewer_key_request{false};
constinit std::atomic<u32> g_source_epoch{0};
constinit std::atomic<u64> g_queue_fence[4] = {};
constinit std::atomic<u32> g_queue_seq{0};
constinit PresentRec g_present_ring[PresentRingSize] = {};

namespace
{
void CaptureInto(GameSurface &surf, const char *who, const NvGraphicBufferRaw *gb, s32 slot_arg)
{
    if (gb == nullptr || gb->num_planes == 0)
    {
        return;
    }
    const NvSurfaceRaw *p0 = std::addressof(gb->planes[0]);

    /* a new buffer object is a new game (or a new swapchain): forget
     * the old slots. Before  the slot was a running count of these
     * calls, so a relaunched game's buffers landed at slots 6 and 7. */
    /* slot 0 starts a set. A different nvmap id alone no longer
     * does - Minecraft registers each slot from its own buffer object, and
     * the old rule restarted the set at every slot. */
    if (slot_arg == 0 || (slot_arg < 0 && static_cast<u32>(gb->nvmap_id) != surf.nvmap_id))
    {
        for (auto &o : surf.slot_offset)
        {
            o = 0;
        }
        for (auto &o : surf.slot_nvmap)
        {
            o = 0;
        }
        surf.num_slots = 0;
        ++surf.generation;
    }
    const u32 slot = (slot_arg >= 0 && slot_arg < 8) ? static_cast<u32>(slot_arg) : surf.num_slots;
    if (slot < 8)
    {
        surf.slot_offset[slot] = p0->offset;
        surf.slot_nvmap[slot] = static_cast<u32>(gb->nvmap_id);
        if (slot + 1 > surf.num_slots)
        {
            surf.num_slots = slot + 1;
        }
    }
    surf.buf_size = gb->total_size;
    surf.color_format = p0->color_format;
    surf.layout = p0->layout;
    surf.nvmap_id = static_cast<u32>(gb->nvmap_id);
    surf.width = p0->width;
    surf.height = p0->height;
    surf.stride_px = p0->pitch / 4;
    surf.block_h_log2 = p0->block_height_log2;
    /* NvColorFormat -> VIC pixel format. A8B8G8R8 is the swapchain default. */
    surf.pix_format = (p0->color_format == 0x0100D12120ULL) ? vic::PIXFMT_A8R8G8B8 : vic::PIXFMT_A8B8G8R8;
    surf.armed = true;

    LogLine("   captured %s slot %u: nvmap=%u %ux%u stride_px=%u blk_h_log2=%u off=0x%x pixfmt=%u (slots=%u)", who,
            slot, surf.nvmap_id, surf.width, surf.height, surf.stride_px, surf.block_h_log2, p0->offset,
            surf.pix_format, surf.num_slots);
}
} // namespace

void CaptureGameSurface(const NvGraphicBufferRaw *gb, s32 slot_arg)
{
    CaptureInto(g_game_surface, "game", gb, slot_arg);
}
void CaptureAppletSurface(const NvGraphicBufferRaw *gb, s32 slot_arg)
{
    CaptureInto(g_applet_surface, "applet", gb, slot_arg);
}

namespace
{

constinit std::atomic<s32> g_vic_pending{-1};
/* RunVicBlit gave the donor's memory back - run it again (it
 * waits for the next viewer) */
constinit std::atomic<bool> g_rearm{false};
constinit s32 g_last_slot = 0;
alignas(os::ThreadStackAlignment) constinit u8 g_vic_stack[24_KB]; /* 32, 10 used streaming with screenshots */
constinit os::ThreadType g_vic_thread;

void RunVicBlit(s32 slot)
{
    /* with home_menu on, no game is needed (the HOME menu streams) */
    bool expected = false;
    if (!g_blit_done.compare_exchange_strong(expected, true))
    {
        return;
    }

    const u32 sidx = (slot >= 0 && static_cast<u32>(slot) < g_game_surface.num_slots) ? static_cast<u32>(slot) : 0;
    const u32 src_off = g_game_surface.slot_offset[sidx];

    /* declared before the first goto: a jump may not bypass an initialised
     * declaration, and close_vic reads these to decide what to unpin. */
    u32 cfg_addr = 0, dst_addr = 0, src_addr = 0, self_addr = 0;

    /*  measurement: in live mode, set up nothing - no heap, no
     * nvdrv, no channels - until a viewer reads. With no viewer the module
     * then holds only its image and static buffers in the Applet group,
     * and the web applet test shows whether that alone fits. */
    for (u32 heap_fails = 0;; ++heap_fails)
    {
        if (heap_fails == 0)
        {
            LogLine("   engines: set up when a viewer connects (until then: no heap, no channels)");
        }
        VicStage("vb:wait_viewer");
        /* a screenshot needs the engines too, viewer or not; neither while
         * an applet such as the browser holds the Applet pool - the donor
         * would take its memory and the applet abort (2210-1024) */
        for (bool said_busy = false;;)
        {
            /* a viewer, and  a game that draws - not the HOME menu */
            for (;;)
            {
                if (UsbViewerPresent(300))
                {
                    /* the HOME menu streams too (home_menu = 1) */
                    break;
                }
                os::SleepThread(TimeSpan::FromMilliSeconds(500));
            }
            u64 mb = 0;
            const bool hold = AppletHoldActive();
            if (!hold && !AppletBusy(std::addressof(mb)))
            {
                break;
            }
            if (!said_busy)
            {
                LogLine("   engines: %s - waiting for it to close",
                        hold ? "an applet is starting" : "an applet holds the Applet pool");
                said_busy = true;
            }
            VicStage("vb:applet_busy");
            SetViewerReason(ViewerReason_AppletOpen);
            os::SleepThread(TimeSpan::FromSeconds(2));
        }
        g_applet_needs_memory.store(false, std::memory_order_relaxed);
        SetViewerReason(ViewerReason_Starting);
        LogLine("   engines: %s - setting up",
                "a viewer is reading");

        VicStage("vb:0_heap");
        if (AllocVicHeap())
        {
            break;
        }
        VicStage("vb:0_FAILED");
        /* no memory from the donor (an applet just closed, or the
         * donor is not installed): wait and try again, not give up for the boot */
        if (heap_fails % 12 == 0)
        {
            LogLine("   engines: no streaming memory - trying again in 5 s");
        }
        SetViewerReason(ViewerReason_NoMemory);
        os::SleepThread(TimeSpan::FromSeconds(5));
    }
    /* the test paths (no live mode, or the colour sweep) use the capture
     * region from here on, for the whole boot, as before  */

    VicStage("vb:1_smGetService");
    /* Service name decides the NvDrvPermission mask, and phys=0 on the
     * game's handle is a permissions story, not a bug:
     *   nvdrv:s (sysmodules) 0x439E  - no bit 10 (SetAruidWithoutCheck),
     *                                  no bit 12 (import EXPORTED handles),
     *                                  no bit 15 (full VA range - and indeed
     *                                  our IOVAs sit in the restricted
     *                                  0xE0000000+ window)
     *   nvdrv:t (factory)    0xFFFFFFFF - everything
     * nvmap objects are bound to an AppletResourceUserId, so importing
     * another process's buffer plausibly needs the aruid bits we lack. */
    ::Result rc = 0;
    const char *nv_svc = "nvdrv:t";
    {
        SmUse sm_nv; /* sm only for this (the live loop never returns) */
        rc = smGetService(std::addressof(g_nv_srv), "nvdrv:t");
        if (R_FAILED(rc))
        {
            LogLine("   nvdrv:t unavailable rc=0x%x - falling back", rc);
            rc = smGetService(std::addressof(g_nv_srv), "nvdrv:s");
            nv_svc = "nvdrv:s";
        }
    }
    LogLine("   using %s rc=0x%x", nv_svc, rc);
    if (R_FAILED(rc))
    {
        VicStage("vb:1_FAILED");
        return;
    }

    VicStage("vb:2_tmem");
    u64 &nv_tmem_size = g_nv_tmem_size;
    if (!DonorTakeNvTmem(std::addressof(g_nv_tmem_handle), std::addressof(nv_tmem_size)))
    {
        LogLine("   nvdrv transfer memory: none came with the donor's memory - no engines");
        VicStage("vb:2_FAILED");
        serviceClose(std::addressof(g_nv_srv));
        return;
    }

    VicStage("vb:3_Initialize");
    {
        const u32 tmem_size = static_cast<u32>(nv_tmem_size);
        rc = serviceDispatchIn(std::addressof(g_nv_srv), 3, tmem_size, .in_num_handles = 2,
                               .in_handles = {CUR_PROCESS_HANDLE, g_nv_tmem_handle});
        LogLine("   Initialize rc=0x%x", rc);
        if (R_FAILED(rc))
        {
            VicStage("vb:3_FAILED");
            goto close_sess;
        }
    }

    u32 nvmap_fd, nverr;

    // Handle ownership does not make another process's NV mappings reusable.
    LogLine("   live mode: our own nvdrv identity (no game aruid, no FROM_ID)");

    VicStage("vb:4_open_nvmap");
    rc = NvOpen("/dev/nvmap", std::addressof(nvmap_fd), std::addressof(nverr));
    LogLine("   /dev/nvmap rc=0x%x fd=%u nverr=%u", rc, nvmap_fd, nverr);
    if (R_FAILED(rc) || nverr != 0)
    {
        VicStage("vb:4_FAILED");
        goto close_sess;
    }

    {
        u32 gfd = 0, gerr = 0;
        const ::Result gr = NvOpen("/dev/nvhost-gpu", std::addressof(gfd), std::addressof(gerr));
        LogLine("   perm probe: /dev/nvhost-gpu rc=0x%x nverr=%u -> %s", gr, gerr,
                (R_SUCCEEDED(gr) && gerr == 0) ? "OPEN (bit0 set: full mask)" : "denied (restricted mask)");
        if (R_SUCCEEDED(gr) && gerr == 0)
        {
            NvClose(gfd);
        }
    }

    u32 src_handle;
    src_handle = 0; /* M86b: stays 0 in live mode (no FROM_ID); assigned, not initialised - gotos above jump past */
    /* --- what did the full mask actually open? -------------------------
     * Reading another process's swapchain is structurally out (nvservices
     * maps client memory through the process handle we gave it at
     * Initialize, and the game's pages are not in our process). So survey
     * the nodes the full mask now reaches - particularly the DISPLAY group,
     * bit 8 - because a post-composition source needs no foreign handle at
     * all, and would capture the home menu and overlays too. */
    {
        VicStage("vb:4b_node_survey");
        static const char *const nodes[] = {
            "/dev/nvhost-display", "/dev/nvdisp-ctrl",  "/dev/nvdisp-disp0",  "/dev/nvdisp-disp1",
            "/dev/nvdcutil-disp0", "/dev/nvcec-ctrl",   "/dev/nvhost-as-gpu", "/dev/nvhost-ctrl-gpu",
            "/dev/nvhost-msenc",   "/dev/nvhost-nvdec", "/dev/nvhost-tsec",   "/dev/nvhost-nvjpg",
        };
        for (const char *n : nodes)
        {
            u32 fd = 0, e = 0;
            const ::Result r = NvOpen(n, std::addressof(fd), std::addressof(e));
            const bool ok = R_SUCCEEDED(r) && e == 0;
            LogLine("   node %-22s %s (rc=0x%x nverr=%u)", n, ok ? "OPEN" : "denied", r, e);
            if (ok)
            {
                NvClose(fd);
            } /* channels are exclusive - never leak one */
        }
    }

    VicStage("vb:5_FROM_ID");
    // Only submit engine addresses returned for buffers mapped by this session.

    u32 cfg_handle, cfg_id, cmd_handle, cmd_id, dst_handle, dst_id, self_handle, self_id;
    VicStage("vb:6_alloc_bufs");
    rc = NvmapOwn(nvmap_fd, g_vic_cfg_buf, VicCfgSize, 0, std::addressof(cfg_handle), std::addressof(cfg_id));
    if (R_FAILED(rc))
    {
        VicStage("vb:6_cfg_FAILED");
        goto close_sess;
    }
    rc = NvmapOwn(nvmap_fd, g_vic_cmd_buf, VicCmdSize, 0, std::addressof(cmd_handle), std::addressof(cmd_id));
    if (R_FAILED(rc))
    {
        VicStage("vb:6_cmd_FAILED");
        goto close_sess;
    }
    rc = NvmapOwn(nvmap_fd, g_vic_dst_buf, VicDstSize, 0, std::addressof(dst_handle), std::addressof(dst_id));
    if (R_FAILED(rc))
    {
        VicStage("vb:6_dst_FAILED");
        goto close_sess;
    }
    rc = NvmapOwn(nvmap_fd, g_vic_src_buf, VicSrcSize, 0, std::addressof(self_handle), std::addressof(self_id));
    if (R_FAILED(rc))
    {
        VicStage("vb:6_self_FAILED");
        goto close_sess;
    }
    AMS_UNUSED(cfg_id, cmd_id, dst_id, self_id);
    AMS_UNUSED(cfg_handle, dst_handle, src_handle, src_off); /* Phase A: relocs disabled */

    u32 vfd, syncpt;
    VicStage("vb:7_open_vic");
    rc = NvOpen("/dev/nvhost-vic", std::addressof(vfd), std::addressof(nverr));
    LogLine("   /dev/nvhost-vic rc=0x%x fd=%u nverr=%u", rc, vfd, nverr);
    if (R_FAILED(rc) || nverr != 0)
    {
        VicStage("vb:7_FAILED");
        goto close_sess;
    }
    {
        struct
        {
            u32 module_id;
            u32 syncpt;
        } gs = {0, 0};
        rc = NvIoctl(vfd, NvHostIocChannelGetSyncpoint, std::addressof(gs), sizeof(gs), std::addressof(nverr));
        LogLine("   GET_SYNCPOINT rc=0x%x nverr=%u -> syncpt=%u", rc, nverr, gs.syncpt);
        if (R_FAILED(rc) || nverr != 0)
        {
            VicStage("vb:7_syncpt_FAILED");
            goto close_vic;
        }
        syncpt = gs.syncpt;
    }

    VicStage("vb:8_set_nvmap_fd");
    {
        struct
        {
            u32 fd;
        } a = {nvmap_fd};
        rc = NvIoctl(vfd, NvHostIocChannelSetNvmapFd, std::addressof(a), sizeof(a), std::addressof(nverr));
        LogLine("   SET_NVMAP_FD(%u) rc=0x%x nverr=%u", nvmap_fd, rc, nverr);
    }
    {
        struct
        {
            u32 timeout;
        } a = {1000};
        rc = NvIoctl(vfd, NvHostIocChannelSetSubmitTo, std::addressof(a), sizeof(a), std::addressof(nverr));
        LogLine("   SET_SUBMIT_TIMEOUT rc=0x%x nverr=%u", rc, nverr);
    }

    /* device address 0 is a real address the channel hands out
     * when it is free - and to every check here it means "not pinned"
     * (a zero address hangs the VIC). One boot gave it to cfg, and every
     * job of that boot was refused: no stream at all. A page of our own,
     * mapped first and never unmapped, takes it instead. It sits in the
     * unused gap between the command buffer and dst. */
    {
        VicStage("vb:8_guard");
        g_guard_nvmap_fd = nvmap_fd;
        u32 gh = 0, gid = 0, gaddr = 0;
        if (R_SUCCEEDED(NvmapOwn(nvmap_fd, reinterpret_cast<u8 *>(g_vic_heap + 0x8000), 0x1000, 0,
                                 std::addressof(gh), std::addressof(gid))))
        {
            MapCmdBuffer(vfd, gh, std::addressof(gaddr), "guard (holds device address 0)");
        }
    }
    VicStage("vb:8a_map_cfg");
    if (!MapCmdBuffer(vfd, cfg_handle, std::addressof(cfg_addr), "cfg"))
    {
        VicStage("vb:8a_FAILED");
        goto close_vic;
    }
    VicStage("vb:8b_map_dst");
    if (!MapCmdBuffer(vfd, dst_handle, std::addressof(dst_addr), "dst"))
    {
        VicStage("vb:8b_FAILED");
        goto close_vic;
    }
    /* The game's imported handle pins with nverr=0 but phys=0. Try every
     * variant before giving up; none of these submit anything.
     * M86b: never in live mode - there is no imported handle. */
    VicStage("vb:8c_map_src");
    if (src_handle != 0)
    {
        MapCmdBuffer(vfd, src_handle, std::addressof(src_addr), "src(game)", 0);
        if (src_addr == 0)
        {
            MapCmdBuffer(vfd, src_handle, std::addressof(src_addr), "src(game)", 1);
        }
        if (src_addr == 0)
        {
            MapCmdBuffer(vfd, src_handle, std::addressof(src_addr), "src(game)", 0, NvHostIocChannelMapCmdBufEx);
        }
    }
    VicStage("vb:8d_map_self");
    MapCmdBuffer(vfd, self_handle, std::addressof(self_addr), "src(ours)", 0);
    LogLine("   pinned: cfg=0x%x dst=0x%x self=0x%x game_src=0x%x (+slot off 0x%x)", cfg_addr, dst_addr, self_addr,
            src_addr, src_off);

    /* Paint a recognisable pattern into the source we own, so a correct
     * blit is provable byte-for-byte rather than just "non-zero". */
    for (u32 y = 0; y < DstH; y++)
    {
        u8 *row = g_vic_src_buf + y * DstPitch;
        for (u32 x = 0; x < DstW; x++)
        {
            row[x * 4 + 0] = 0xFF;                   /* A */
            row[x * 4 + 1] = static_cast<u8>(x * 4); /* R ramps across */
            row[x * 4 + 2] = static_cast<u8>(y * 4); /* G ramps down   */
            row[x * 4 + 3] = 0x11;                   /* B constant     */
        }
    }
    armDCacheFlush(g_vic_src_buf, VicSrcSize);
    LogLine("   self-src pattern: row0[0..8]=%02x %02x %02x %02x %02x %02x %02x %02x", g_vic_src_buf[0],
            g_vic_src_buf[1], g_vic_src_buf[2], g_vic_src_buf[3], g_vic_src_buf[4], g_vic_src_buf[5],
            g_vic_src_buf[6], g_vic_src_buf[7]);

    {
        const SrcDesc game_src{g_game_surface.width,        g_game_surface.height,       g_game_surface.stride_px,
                               vic::BLK_KIND_GENERIC_16Bx2, g_game_surface.block_h_log2, g_game_surface.pix_format,
                               vic::CACHE_WIDTH_64Bx4,      g_game_surface.width,        g_game_surface.height};
        const JobCtx ctx{vfd, cmd_handle, syncpt,     cfg_addr,   dst_addr, (self_addr != 0) ? self_addr : src_addr,
                         0,   cfg_handle, dst_handle, src_handle, game_src};
        /* Once the VIC stops completing it is wedged; further submits achieve
         * nothing and keep the compositor starved. Stop at the first failure. */

        /* 1. Control: known good since . */
        if (!RunOneJob("vb:job_fill", VicJob::Fill, true, ctx))
        {
            LogLine("   fill did not complete - engine wedged, skipping the rest");
            VicStage("vb:ABORTED_after_fill");
        }
        else if (self_addr == 0)
        {
            LogLine("   no address for our own source buffer; nothing further to try");
            VicStage("vb:no_self_addr");
        }
        else
        {
            /* 2. A genuine blit from a buffer WE own, so every address the
             *    engine touches is one MAP_CMD_BUFFER actually gave us. */
            RunOneJob("vb:job_blit_self", VicJob::BlitSelf, true, ctx);
        }

        /* The game's buffer stays out of reach: its handle pins to phys=0
         * three different ways, and RunOneJob now refuses zero addresses. */
        if (src_addr == 0)
        {
            LogLine("   game blit still not possible: handle pins to phys=0");
            VicStage("vb:game_blit_SKIPPED");
        }
    }
    VicStage("vb:ALL_JOBS_DONE");

    /* VIC arithmetic on our own buffers, before anything touches the
     * game or another engine */

    /* M86b: the vi:m indirect-layer probe is a closed route  and
     * touches the display service; live mode skips it */
    RunLive(vfd, nvmap_fd, cmd_handle, syncpt, cfg_addr);
    /* gated. This has completed cleanly in every run (its cmdbuf is an
     * IMMEDIATE increment with no engine op), but the point of an observer
     * run is to touch no engine channel at all. */
    /* the observer goes first. It touches no engine, and this way it
     * reads grc's status blocks before any job of ours has shared NVENC
     * with grc. */
    /* the positive control goes first among the engine jobs. If it
     * wedges the engine, nothing after it would be informative anyway. */

close_vic:
    if (cfg_addr != 0)
    {
        UnmapCmdBuffer(vfd, cfg_handle);
    }
    if (dst_addr != 0)
    {
        UnmapCmdBuffer(vfd, dst_handle);
    }
    if (src_addr != 0)
    {
        UnmapCmdBuffer(vfd, src_handle);
    }
    if (self_addr != 0)
    {
        UnmapCmdBuffer(vfd, self_handle);
    }
    NvClose(vfd);
close_sess:
    serviceClose(std::addressof(g_nv_srv));
    /* back with the donor's memory: closed when the donor goes, or used
     * again if that memory is kept for the next stream (an unmap refused) */
    if (g_nv_tmem_handle != os::InvalidNativeHandle)
    {
        DonorAcceptNv(g_nv_tmem_size, g_nv_tmem_handle);
        g_nv_tmem_handle = os::InvalidNativeHandle;
    }
    VicStage("vb:released");
    /* closing the nvdrv session freed every fd and nvmap handle
     * on the donor's memory; unmap it, end the donor, and let the worker
     * set up again for the next viewer */
    if (g_donor_mode)
    {
        DonorRelease();
        g_donor_mode = false;
        g_vic_heap = 0;
        g_vic_heap_size = 0;
        g_vic_cfg_buf = g_vic_cmd_buf = g_vic_dst_buf = g_vic_src_buf = nullptr;
        g_ind_buf = nullptr;

        g_ind_size = 0;
        g_stage_buf = nullptr;
        g_stream_stage[0] = g_stream_stage[1] = nullptr;
        g_ctrl_fd = 0;
        g_guard_nvmap_fd = 0;
        g_guard_next = 0x9000;
        g_blit_done.store(false);
        g_rearm.store(true);
        LogLine("   engines closed, the donor's memory given back - set up again when a viewer connects");
    }
}

/* ---- vi indirect-layer capture ------------------------------------
 * The only mechanism that can satisfy our constraint: 2450 writes the
 * layer image into a type-0x46 buffer, i.e. memory WE supply. Exact
 * marshalling taken from libnx viGetIndirectLayerImageMap, and our 2460
 * call already matched libnx byte-for-byte and returned rc=0.
 *
 * 102 was denied earlier only because the game's session is vi:u with
 * mode 0. Per switchbrew, "passing 1 to vi:s/vi:m results in the
 * IApplicationDisplayService having greater privileges" - so we open
 * vi:m ourselves and ask for mode 1. */

// Attach only to locate/read buffers; keep the process running and release the debug handle on exit.

/* The swapchain is ONE nvmap object: three slots of 8,847,360 B, laid out
 * contiguously at the offsets the binder parcels already handed us. */
constexpr u64 FbSlotSize = 8847360ull;
constexpr u64 FbSwapSize = 3ull * FbSlotSize; /* 26,542,080 */

/* One block-row: 120 blocks x 8192 B, covering the full 1920 px width by
 * 128 rows, contiguous. Nine of them are exactly one 8,847,360 B slot. */
constexpr u64 FbBlockRow = 983040ull;

/*  found it. Region base 0x10851e6000, size EXACTLY 26,542,080 - three
 * slots, and the same size nvmap PARAM reported for handle 1268 - with the
 * opaque-pixel signature at all three slot offsets:
 *
 *   slot0 ff fb ff ff | slot1 ff f7 ff ff | slot2 ff f4 ff ff
 *
 * Same lane constant, the varying lane differing per slot: three
 * successive frames of a near-white top-left corner. That is the
 * swapchain. Exactly one candidate survived ~24,000 probed offsets.
 *
 * Two things  got wrong, fixed here:
 *   - it reported the offset against big[0] rather than the region the
 *     candidate was actually found in (cosmetic, but it read as 0x11d79000
 *     when the true answer is offset 0 of an exactly-sized region);
 *   - it scanned 24,000 offsets and burned its whole read budget when an
 *     exactly-26,542,080-byte region is a dead giveaway. Check those
 *     first: the freeze drops from ~340 ms to ~1 ms.
 *
 * ADDRESSES ARE NOT STABLE. The same region was at 0x14f5e6d000 one boot
 * and 0x107346d000 the next. It must be located at runtime, every time. */
/*  matched on an all-0xFF corner, so every lane looked "opaque" and
 * lane 0 won by being first. The real alpha lane is 3: the captured
 * strip reads aa aa af ff / ab ab af ff, i.e. A8B8G8R8 stored
 * little-endian as R,G,B,A. Hence "opaque pixels 0/245760" - the stat
 * counted lane 0, which is red. Two fixes: the fallback scan now really
 * does reject a uniform block ( computed `varied` and never acted on
 * it), and the alpha lane is measured from the strip rather than trusted
 * from 16 bytes of white. */

/* Stream one whole slot out to the SD card, 64 KB at a time, while the
 * game runs. 8,847,360 B = 120 blocks x 8192 B x 9 block-rows = a
 * 1920x1152 block-linear surface, of which the top 1080 rows are the
 * frame. De-swizzling happens on the PC (tools/deswizzle.py). */

// Swapchain addresses change between processes and boots; locate them at runtime.

/* `src` must be normal cached memory; `verify` is scratch of at least
 * `len` bytes, also cached. One retry on any failure. */

/* For engine output: copy out of the uncached nvmap buffer into the
 * cached stage buffer first, then write. */

/* ---- : the streaming prototype --------------------------------
 * A composition of proven pieces, not new engine work: the debug-SVC
 * slot read (5.6 ms at 1571 MB/s, ), the VIC scaled blit (the BEST
 * variant already does a uniform 4x downscale, ), and the USB bulk
 * transport (, byte-exact).
 *
 * LATENCY is the design goal, so the loop never queues. Each iteration
 * takes the slot the game has JUST presented and discards whatever
 * arrived while we were busy - a backlog would only add delay, and a
 * dropped frame is cheaper than a late one.
 *
 * THROUGHPUT comes from overlap. The post is asynchronous, so frame N's
 * transfer runs concurrently with frame N+1's capture. Serially 480x270
 * costs 5.6 + vic + copy + 12.4 ms and caps near 43 fps; overlapped the
 * transfer alone is the bottleneck and 60 fps fits.
 *
 * g_vic_dst_buf is UNCACHED nvmap memory - handing that to any I/O path
 * is what made every VIC dump fail in / - so each frame is staged
 * into cached memory that already carries its header, giving one URB
 * per frame rather than two. */
struct __attribute__((packed)) SftHdrWire
{
    u32 magic;
    u16 version;
    u16 flags;
    u32 width, height, stride, length, block_h_log2, kind;
};
static_assert(sizeof(SftHdrWire) == 32, "must match nxrp_hdr_t in tools/raw-recv/raw-recv.c");

/* Retained as a fallback: it needs no engine, no pin and no syncpoint,
 * so it still works if the VIC is ever unavailable.  streams through
 * the VIC instead - 1.5 bytes/pixel against this one's 4, and 0.8 ms of
 * engine time against ~4.3 ms of CPU. */

// Reattach after source changes and stop a stream when its viewer leaves.
bool FindSwapchainBySize(::ams::svc::Handle dbg, u64 want, u64 *out)
{
    u64 addr = 0, best = 0, best_size = ~UINT64_C(0);
    for (u32 steps = 0; steps < 4000; ++steps)
    {
        ::ams::svc::MemoryInfo mi = {};
        ::ams::svc::PageInfo pi = {};
        if (!DbgQuery(dbg, std::addressof(mi), std::addressof(pi), addr) || mi.size == 0)
        {
            return false;
        }
        const u32 attr = static_cast<u32>(mi.attribute);
        const bool dev = (attr & ::ams::svc::MemoryAttribute_DeviceShared) != 0 || mi.device_count > 0;
        if (dev && mi.size == want)
        {
            *out = mi.base_address;
            return true;
        }
        /* some homebrew keeps its framebuffers inside a larger
         * device-mapped block (for example two 1280x720 slots at +0xF0000
         * and +0x4B0000 of one allocation) - the smallest such block
         * that can hold them, if no region is exactly their size */
        if (dev && mi.size > want && mi.size < best_size)
        {
            best = mi.base_address;
            best_size = mi.size;
        }
        const u64 next = mi.base_address + mi.size;
        if (next <= addr)
        {
            break;
        }
        addr = next;
    }
    if (best_size != ~UINT64_C(0))
    {
        LogLine(
            "   swapchain: no region of exactly %llu B; using the smallest device-mapped one that holds it (%llu B)",
            static_cast<unsigned long long>(want), static_cast<unsigned long long>(best_size));
        *out = best;
        return true;
    }
    return false;
}

/* Where each slot's buffer may be, when no region is exactly the
 * swapchain (FindSwapchainBySize fell back to "the smallest region
 * that holds it", which in a homebrew app is as likely its texture
 * memory - Moonlight streamed its box art). Which candidate is which
 * slot is worked out while streaming (NvfSession::Calibrate).
 *
 * One nvmap object per slot (GameSurface::separate - Smash Ultimate,
 * Moonlight): every device-mapped region that is a whole number of
 * buffers, each rounded up to a power-of-two alignment (Moonlight
 * keeps a 3.75 MB 720p buffer in a 4 MB block), gives one candidate
 * per buffer - adjacent buffers can share one memory block.
 * One object for every slot (DuckStation: two slots at 0 and
 * 0x3C0000): every device-mapped region that holds the whole set
 * gives one candidate per slot, its base plus that slot's offset. */
u32 FindSlotCandidates(::ams::svc::Handle dbg, u64 buf, const u32 *slot_off, u32 nslots, bool separate, u64 *out,
                       u32 max, u32 *devc = nullptr)
{
    if (buf == 0 || nslots == 0)
    {
        return 0;
    }
    static constexpr u64 Aligns[] = {0x1000, 0x10000, 0x20000, 0x40000, 0x100000, 0x200000, 0x400000};
    u64 span = buf;
    for (u32 k = 0; k < nslots; ++k)
    {
        if (slot_off[k] + buf > span)
        {
            span = slot_off[k] + buf;
        }
    }
    u64 addr = 0;
    u32 n = 0;
    for (u32 steps = 0; steps < 4000 && n < max; ++steps)
    {
        ::ams::svc::MemoryInfo mi = {};
        ::ams::svc::PageInfo pi = {};
        if (!DbgQuery(dbg, std::addressof(mi), std::addressof(pi), addr) || mi.size == 0)
        {
            break;
        }
        const u32 attr = static_cast<u32>(mi.attribute);
        const bool dev = (attr & ::ams::svc::MemoryAttribute_DeviceShared) != 0 || mi.device_count > 0;
        if (dev && separate)
        {
            for (const u64 a : Aligns)
            {
                const u64 step = (buf + a - 1) & ~(a - 1);
                if (mi.size % step == 0 && mi.size / step <= 8)
                {
                    for (u64 o = 0; o < mi.size && n < max; o += step)
                    {
                        if (devc)
                        {
                            devc[n] = mi.device_count;
                        }
                        out[n++] = mi.base_address + o;
                    }
                    break;
                }
            }
        }
        else if (dev && mi.size >= span)
        {
            /* what kind of region - Sonic Frontiers has two 640 MB
             * pools laid out alike, and the matcher once took the wrong one */
            LogLine("     region 0x%010llx, %llu MB: state 0x%x attr 0x%x perm 0x%x, device mappings %u, ipc %u",
                    static_cast<unsigned long long>(mi.base_address), static_cast<unsigned long long>(mi.size >> 20),
                    static_cast<u32>(mi.state), attr, static_cast<u32>(mi.permission), mi.device_count, mi.ipc_count);
            for (u32 k = 0; k < nslots && n < max; ++k)
            {
                if (devc)
                {
                    devc[n] = mi.device_count;
                }
                out[n++] = mi.base_address + slot_off[k];
            }
        }
        const u64 next = mi.base_address + mi.size;
        if (next <= addr)
        {
            break;
        }
        addr = next;
    }
    return n;
}

/* the region the swapchain is in: exactly the set's size (MK8, Smash's
 * first slot), or the smallest that holds it. When that is not the
 * whole story (one object per slot, homebrew in larger blocks) the
 * stream's NvfSession::PrepareSlots finds the slot buffers and matches
 * them while streaming (, from WillMidia's PR #4 - it replaces
 * 's calibration before the stream, which held the picture back
 * 2-6 s and never started on a still screen). */
bool LocateSwapchain(::ams::svc::Handle dbg, GameSurface &geo, u64 *base, const char *who)
{
    AMS_UNUSED(who);
    u64 want = FbSwapSize;
    if (geo.num_slots != 0 && geo.buf_size != 0)
    {
        u32 top = 0;
        for (u32 k = 0; k < geo.num_slots && k < 8; ++k)
        {
            if (geo.slot_offset[k] > top)
            {
                top = geo.slot_offset[k];
            }
        }
        want = static_cast<u64>(top) + geo.buf_size;
    }
    return FindSwapchainBySize(dbg, want, base);
}

void RunLive(u32 vfd, u32 nvmap_fd, u32 cmd_handle, u32 syncpt, u32 cfg_addr)
{
    LogLine("   ==== LIVE MODE: streaming whenever a viewer is reading and a game is running ====");
    /* a release install captures only on the firmware it was
     * tested on (22.x; : 21.x too, a user's 21.0.1; :
     * 23.x, a user's 23.0.0); the debug reads and engine jobs assume it */
    {
        const u32 v = hosversionGet();
        if ((HOSVER_MAJOR(v) < 21 || HOSVER_MAJOR(v) > 23))
        {
            LogLine(
                "   live: firmware %u.%u.%u is untested (this build: 21.x-23.x) - not streaming. allow_untested_firmware = 1 in config.ini overrides.",
                HOSVER_MAJOR(v), HOSVER_MINOR(v), HOSVER_MICRO(v));
            VicStage("live:untested_fw");
            SetViewerReason(ViewerReason_Unsupported);
            for (;;)
            {
                static_cast<void>(UsbViewerPresent(300));
                os::SleepThread(TimeSpan::FromSeconds(2));
            }
        }
    }
    u64 skip_pid = 0; /* a game whose swapchain was not found: wait for another */
    bool said_wait_viewer = false;
    u32 sessions = 0;
    u64 selection_log = 0;
    bool said_off = false;
    /* with no viewer for this long, close the engines and give
     * the donor's memory back (RunVicBlit tears down and sets up again
     * for the next viewer) - the browser applets need that pool */
    constexpr u64 IdleTeardownNs = 15'000'000'000ull;
    u64 idle_since = 0;
    auto idle_long = [&]() -> bool {
        const u64 now = armTicksToNs(armGetSystemTick());
        if (idle_since == 0)
        {
            idle_since = now;
        }
        return g_donor_mode && now - idle_since >= IdleTeardownNs && g_ind_buf == nullptr && g_capture_handles == 0;
    };
    for (;;)
    {
        if (g_engine_wedged)
        {
            VicStage("live:engine_wedged");
            LogLine("   live: the engine is wedged - no more streaming this boot");
            SetViewerReason(ViewerReason_Wedged);
            for (;;)
            {
                static_cast<void>(UsbViewerPresent(300));
                os::SleepThread(TimeSpan::FromSeconds(2));
            }
        }
        /* an applet is starting - give the donor's memory back now */
        if (g_applet_needs_memory.load(std::memory_order_relaxed) && g_donor_mode)
        {
            CaptureRelease("live");
            if (g_ind_buf == nullptr && g_capture_handles == 0)
            {
                LogLine("   live: an applet needs the memory - closing the engines now");
                return;
            }
        }
        /* a screenshot while not streaming: attach just for it */
        /* switched off from the overlay - not even a USB probe */
        if (said_off)
        {
            LogLine("   live: turned on (overlay)");
            said_off = false;
            said_wait_viewer = false;
        }
        if (!UsbViewerPresent(300))
        {
            NxrpSourceReset();
            if (!said_wait_viewer)
            {
                LogLine("   live: waiting for a viewer (raw-view) to read the stream");
                VicStage("live:no_viewer");
                said_wait_viewer = true;
            }
            CaptureRelease("live");
            if (idle_long())
            {
                LogLine("   live: no viewer for 15 s - closing the engines, memory back to the system");
                return;
            }
            os::SleepThread(TimeSpan::FromMilliSeconds(700));
            continue;
        }
        said_wait_viewer = false;
        if ((armTicksToNs(armGetSystemTick() - selection_log) >= 2000000000ULL))
        {
            selection_log = armGetSystemTick();
            HybridLogState();
            const u64 qt = g_queue_tick.load(std::memory_order_relaxed);
            LogLine(
                "select source_state=%u queue=%u presenter=%llu queue_age_ms=%llu ui=%u region_kb=%zu stages_kb=%zu",
                NxrpSourceState(), g_queue_count.load(), static_cast<unsigned long long>(g_present_pid.load()),
                static_cast<unsigned long long>(qt ? armTicksToNs(armGetSystemTick() - qt) / 1000000 : 0),
                0u, g_ind_size / 1024, StreamStageSize / 1024);
        }
        if ((NxrpSourceDone()))
        {
            CaptureRelease("NXRP hybrid bounded done");
            if (idle_long())
                return;
            os::SleepThread(TimeSpan::FromMilliSeconds(100));
            continue;
        }
        if ((!HybridGameFocused()))
        {
            if (NxrpSourceDone())
            {
                CaptureRelease("NXRP bounded done");
                if (idle_long())
                    return;
                os::SleepThread(TimeSpan::FromMilliSeconds(100));
                continue;
            }
            idle_since = 0;
            if (!CaptureAcquire("NXRP Default source"))
            {
                os::SleepThread(TimeSpan::FromSeconds(1));
                continue;
            }
            LogLine("NXRP source=composed (HOME/system/unknown focus), shared7200capture budget");
            const StreamEnd result = TryNvencStream(::ams::svc::InvalidHandle, 0, vfd, nvmap_fd, cmd_handle, syncpt,
                                                    cfg_addr, nullptr, true);
            LogLine("NXRP bounded session ended status=%u", static_cast<u32>(result));
            CaptureRelease("NXRP session");
            os::SleepThread(result == StreamEnd::Reconfigure ? TimeSpan::FromMilliSeconds(5)
                                                             : TimeSpan::FromSeconds(1));
            continue;
        }
        idle_since = 0;

        ::ams::os::ProcessId pid{};
        const u32 q_before = g_queue_count.load(std::memory_order_relaxed);
        const bool have_app =
            g_pmdmnt_rc == 0 && R_SUCCEEDED(::ams::pm::dmnt::GetApplicationProcessId(std::addressof(pid)));
        /* the Album / hbmenu draws with buffers of its own, like a game */
        const u64 applet_pid = (g_applet_target_pid.load(std::memory_order_relaxed));
        if ((have_app && pid.value != skip_pid) || applet_pid != 0)
        {
            /* presenting? a suspended or loading-from-boot game is not */
            os::SleepThread(TimeSpan::FromMilliSeconds(200));
        }
        bool presenting = g_queue_count.load(std::memory_order_relaxed) != q_before;
        if ((have_app) && g_present_pid.load() == pid.value)
        {
            const u64 qt = g_queue_tick.load(std::memory_order_relaxed);
            presenting = presenting || (qt && armTicksToNs(armGetSystemTick() - qt) < 1000000000ULL);
        }
        /* whose frames they were - the Album in front of a
         * suspended game presents, the game does not */
        bool applet_target = false;
        if (presenting && applet_pid != 0)
        {
            const u64 presenter = g_present_pid.load(std::memory_order_relaxed);
            if (presenter == applet_pid && (!have_app || presenter != pid.value))
            {
                pid.value = applet_pid;
                applet_target = true;
            }
            else if (!have_app || presenter != pid.value)
            {
                presenting = false;
            }
        }
        const bool have_target = have_app || applet_target;
        /* an app on the never-attach list (anti-debug builds
         * such as TiCo refuse to run under a debugger) */
        if (!have_target || pid.value == skip_pid || !presenting)
        {
            {
                os::SleepThread(TimeSpan::FromMilliSeconds(20));
                continue;
            }
        }

        /* the capture region only now - a viewer reads and a
         * game presents. Short of memory (an applet such as the web
         * browser holds the Applet pool): wait and try again rather
         * than take what the applet needs. */
        if (!CaptureAcquire("live"))
        {
            static u32 short_n = 0;
            if ((short_n++ % 15) == 0)
            {
                LogLine("   live: not enough memory free to stream right now - trying again");
            }
            VicStage("live:no_memory");
            SetViewerReason(ViewerReason_NoMemory);
            os::SleepThread(TimeSpan::FromSeconds(2));
            continue;
        }

        SetViewerReason(ViewerReason_Starting);
        ++sessions;
        ::ams::svc::Handle dbg = ::ams::svc::InvalidHandle;
        if (!DmAttach(pid.value, std::addressof(dbg)))
        {
            LogLine("   live[%u]: no attach to pid %llu (direct failed, dmnt not holding it) - retrying in 5 s",
                    sessions, static_cast<unsigned long long>(pid.value));
            os::SleepThread(TimeSpan::FromSeconds(5));
            continue;
        }
        LogLine("NXRP source=tap pid=%llu; AM focus=1", static_cast<unsigned long long>(pid.value));
        /* this game's geometry, as its setPreallocatedBuffer
         * parcels described it; a consistent copy (the binder thread
         * writes it) */
        GameSurface geo;
        const GameSurface &src_surface = applet_target ? g_applet_surface : g_game_surface;
        g_session_surface = std::addressof(src_surface);
        for (u32 tries = 0; tries < 10; ++tries)
        {
            const u32 gen = src_surface.generation;
            geo = src_surface;
            if (gen == src_surface.generation)
            {
                break;
            }
        }
        if (applet_target)
        {
            LogLine("   live[%u]: the Album / hbmenu is in front (pid %llu)", sessions,
                    static_cast<unsigned long long>(pid.value));
        }
        u64 slot_base = 0;
        const u64 f0 = armTicksToNs(armGetSystemTick());
        const bool found = LocateSwapchain(dbg, geo, std::addressof(slot_base), "live");
        u64 want = FbSwapSize;
        if (geo.num_slots != 0 && geo.buf_size != 0)
        {
            u32 top = 0;
            for (u32 k = 0; k < geo.num_slots && k < 8; ++k)
            {
                if (geo.slot_offset[k] > top)
                {
                    top = geo.slot_offset[k];
                }
            }
            want = static_cast<u64>(top) + geo.buf_size;
        }
        /* A direct attach stopped the game: drain the attach burst and
         * continue. Through dmnt:cht the game never stopped (dmnt pumps
         * its own handle), so DbgResume is a no-op there. */
        u32 nev = 0;
        const Result rc = DbgResume(dbg, std::addressof(nev));
        const u64 frozen_us = (armTicksToNs(armGetSystemTick()) - f0) / 1000;
        LogLine(
            "   live[%u]: attached to pid %llu; %ux%u, %u slot(s); swapchain (%llu B) %s%010llx; game held %llu us; %u attach events, continue rc=0x%x",
            sessions, static_cast<unsigned long long>(pid.value), geo.width, geo.height, geo.num_slots,
            static_cast<unsigned long long>(want), found ? "at 0x" : "NOT FOUND ",
            static_cast<unsigned long long>(slot_base), static_cast<unsigned long long>(frozen_us), nev, rc.GetValue());
        if (R_FAILED(rc))
        {
            DbgDetach(dbg);
            os::SleepThread(TimeSpan::FromSeconds(2));
            continue;
        }
        if (!found)
        {
            DbgDetach(dbg);
            skip_pid = pid.value;
            continue;
        }
        if (!DbgIsDmnt(dbg))
        {
            DebugPumpStart(dbg);
        }

        const GameSurface *gp = (geo.num_slots != 0 && geo.buf_size != 0) ? std::addressof(geo) : nullptr;
        const StreamEnd end = TryNvencStream(dbg, slot_base, vfd, nvmap_fd, cmd_handle, syncpt, cfg_addr, gp);
        if (!DbgIsDmnt(dbg))
        {
            DebugPumpStop();
        }
        DbgDetach(dbg);
        static const char *const names[] = {"done",
                                            "the game went away",
                                            "the viewer went away",
                                            "NVENC stalled",
                                            "failed",
                                            "the picture size changed - restarting at the new size",
                                            "turned off from the overlay"};
        LogLine("   live[%u]: session ended - %s; detached", sessions, names[static_cast<int>(end)]);
        if (end == StreamEnd::Failed)
        {
            os::SleepThread(TimeSpan::FromSeconds(5));
        }
        else if (end != StreamEnd::Reconfigure && end != StreamEnd::Disabled)
        {
            os::SleepThread(TimeSpan::FromSeconds(1));
        }
        else if (end == StreamEnd::Reconfigure)
        {
            os::SleepThread(TimeSpan::FromMilliSeconds((5)));
        } /* a new buffer set registers slot by slot */
    }
}

/* the NVENC ladder.
 *
 *  swept candidate magics and learned nothing, because every case -
 * including a deliberately invalid control - behaved identically. That
 * is the signature of a job that never runs, and no amount of varying
 * the CONTENTS of a job that never runs will say why.
 *
 * So stop varying the content and vary the STRUCTURE instead. Five
 * submits on one channel, each adding exactly one layer:
 *
 *   L0  bare INCR_SYNCPT, IMMEDIATE      does host1x run our cmdbuf here?
 *   L1  + SETCL to class 0x21            does the class switch survive?
 *   L2  + one method write               are methods accepted?
 *   L3  + full method set + EXECUTE      is the job handed to the engine?
 *   L4  same, but INCR on OP_DONE        does the engine COMPLETE it?
 *
 * L0-L3 end with an IMMEDIATE increment, which host1x performs as it
 * retires the opcode regardless of what the engine does. So a fence that
 * moves means "host1x got this far". The first level whose fence does
 * NOT move is the layer that is broken, and that is a fact rather than
 * an inference. If all of L0-L3 pass and only L4 stalls, the engine is
 * receiving fully-formed work and failing to finish it - which is a
 * completely different problem from the one we have been chasing.
 *
 * L3/L4 carry a real, fully populated H.264 all-intra IDR setup with
 * every surface the engine can dereference, at a deliberately tiny
 * 256x128 so the encode is trivial. This is tested inside the ladder rather
 * than as a separate run. */
constexpr u32 NvEncW = 256; /* 16 MBs across */
constexpr u32 NvEncH = 128; /*  8 MBs down   */

/* all 256-aligned: every method below takes an address >> 8 */
constexpr u32 EncInYOff = 0x060000;
constexpr u32 EncInUVOff = EncInYOff + NvEncW * NvEncH;
constexpr u32 EncRefYOff = EncInUVOff + NvEncW * NvEncH / 2;
constexpr u32 EncRefUVOff = EncRefYOff + NvEncW * NvEncH;
constexpr u32 EncEnd = EncRefUVOff + NvEncW * NvEncH / 2;
static_assert(EncEnd <= VicDstSize, "NVENC probe surfaces do not fit the VIC dst buffer");

constexpr u32 SetupRc = __builtin_offsetof(nvenc_h264_drv_pic_setup_s, rate_control);
constexpr u32 SetupRcHrdType = SetupRc + __builtin_offsetof(nvenc_h264_rc_s, hrd_type);
constexpr u32 SetupRcTwoPass = SetupRc + __builtin_offsetof(nvenc_h264_rc_s, two_pass_rc);
static_assert(SetupRcHrdType == 0x70 && SetupRcTwoPass == 0xBA, "nvsetup-dump reads these offsets");
static_assert(nvenc_grc_idr::Setup[SetupRcHrdType] == 1 && nvenc_grc_idr::Setup[SetupRcTwoPass] == 1,
              "grc's IDR setup has VCL HRD and two_pass_rc 1");

// NVENC input and reference planes must use the engine's block-linear layout.
constexpr u32 NvfW = 1280, NvfH = 720;
constexpr OutDesc NvfOut{NvfW, NvfH, NvfW, vic::PIXFMT_Y8_U8V8_N420, vic::BLK_KIND_GENERIC_16Bx2, 1};
constexpr u32 NvfLuma = Nv12LumaBytes(NvfOut), NvfChroma = Nv12ChromaBytes(NvfOut);
constexpr u32 NvfOffSetup = 0x000000;
constexpr u32 NvfOffStatus = 0x001000;
constexpr u32 NvfOffRc = 0x002000;
constexpr u32 NvfOffCmd = 0x006000;
constexpr u32 NvfOffBits = 0x010000;
constexpr u32 NvfBitsSize = 0x160000;
constexpr u32 NvfOffHist = NvfOffBits + NvfBitsSize;
constexpr u32 NvfOffCur = NvfOffHist + 0x0C0000;
constexpr u32 NvfOffCurUV = NvfOffCur + NvfLuma;
constexpr u32 NvfOffRefOut = (NvfOffCurUV + NvfChroma + 0xFFF) & ~0xFFFu;
constexpr u32 NvfArenaSize = NvfOffRefOut + 0x160000;
constexpr size_t NvfArenaBase = FbBlockRowStage + 3 * StreamStageSize; /* past both stream stages */
/* the live capture may use the first stage (g_stage_buf, which
 * the live stream never touches) as the tail of a larger slot */
constexpr size_t MaxSlotBytes = FbBlockRowStage + StreamStageSize;
static_assert(NvfLuma == 1280 * 720 && NvfChroma == 1280 * 368, "16-row blocks: what NVENC reads");
static_assert(NvfOffCurUV % 0x100 == 0 && NvfArenaBase % 0x1000 == 0);
static_assert(sizeof(nvenc_grc_idr::Setup) <= NvfOffStatus && sizeof(nvenc_pic_stat_s) <= NvfOffRc - NvfOffStatus);
constexpr u32 NvfPictureIndex = 0x4D383300u;                                      /* "\0" + n */
constexpr u32 SetupRcQpI = SetupRc + __builtin_offsetof(nvenc_h264_rc_s, QP) + 2; /* QP[P,B,I] */
static_assert(SetupRcQpI == 0x73 && nvenc_grc_idr::Setup[SetupRcQpI] == 24, "grc's I-frame QP is 24");
constexpr u32 SetupIdrPicId = __builtin_offsetof(nvenc_h264_drv_pic_setup_s, pic_control) +
                              __builtin_offsetof(nvenc_h264_pic_control_s, idr_pic_id);
static_assert(SetupIdrPicId == 0x180 && nvenc_grc_idr::Setup[SetupIdrPicId] == 0,
              "tools/nvenc_replay.py reads idr_pic_id at 0x180; grc's setup_8 has 0");

/* the production matrix: RGB -> BT.709 limited range (tools/vic_csc.py gen) */
constexpr CscProbe NvfBt709 = {
    "bt709",
    1,
    vic_csc::Shift,
    {{vic_csc::Bt709[0][0], vic_csc::Bt709[0][1], vic_csc::Bt709[0][2], vic_csc::Bt709[0][3]},
     {vic_csc::Bt709[1][0], vic_csc::Bt709[1][1], vic_csc::Bt709[1][2], vic_csc::Bt709[1][3]},
     {vic_csc::Bt709[2][0], vic_csc::Bt709[2][1], vic_csc::Bt709[2][2], vic_csc::Bt709[2][3]}}};

/* Handheld, MK8 renders 1280x720 into the top-left of the same
 * 1920x1080 surface . That corner is block-rows 0-5 (128 rows
 * each) and, in each, the first 80 of 120 GOB columns (1280 px x 4 B /
 * 64 B) - 655,360 contiguous bytes per block-row. Reading just those is
 * 3.9 MB instead of 8.8 MB, and the VIC samples only that rectangle. */
constexpr u64 CornerBlockRowBytes = 80ull * 8192;
constexpr u32 CornerBlockRows = 6;
static_assert(CornerBlockRows * 128 >= NvfH && CornerBlockRowBytes * 120 / 80 == FbBlockRow);

/* everything size-dependent about an encode arena, so one set of
 * code drives 720p (the constants above, unchanged) and 1080p. */
struct NvfLayout
{
    u32 w, h;
    OutDesc out;      /* what the VIC writes for NVENC */
    u32 off_setup, off_status, off_rc, off_cmd, off_bits, off_hist;
    u32 off_cur, off_cur_uv, off_ref_out, arena_size;
    u32 off_setup_p, off_ref_b, off_me_a, off_me_b, arena_size_p; /* P frames */
    const u8 *idr_setup, *p_setup;
    const u8 *hdrs;
    u32 hdrs_len;
};
const NvfLayout &Layout720(); /* defined past the P-frame constants */

struct NvfCtx
{
    u8 *a;                          /* arena, CPU view (cached) */
    u32 ah;                         /* its nvmap handle */
    u32 efd, esyncpt;               /* msenc channel */
    u32 enc;                        /* arena IOVA in the msenc channel */
};

/* Where one job's setup and references live in the arena. The default
 * is the IDR job; a P job ( nvp) adds grc's three P-only bindings. */
struct NvfJob
{
    u32 setup = NvfOffSetup;
    u32 ref_out = NvfOffRefOut;
    u32 ref_in = 0; /* 0: IDR - no ME or reference inputs */
    u32 me_in = 0, me_out = 0;
    u32 cur = 0, cur_uv = 0; /* 0 = the layout's picture */
    u32 control = 0x00001103u; /* SET_CONTROL_PARAMS: H.264, RCMODE 0 (constant QP) */
};

constinit bool g_nvf_last_fence_done = false, g_nvf_last_ours = false;
constinit u32 g_nvf_last_fence = 0, g_nvf_last_pic = 0;

// Hardware rate control; three failed frames fall back to constant QP.
constexpr u32 NvfControlConstQp = 0x00001103u;
constexpr u32 NvfControlHwRc = 0x12001103u;
struct HwRcConfig
{
    u32 fps = 60, cpb_ms = 250, min_qp = 16, max_qp = 51, start_qp = 30, gop = 240;
};

/* Writes the rate-control block of a 4 KiB setup for `bitrate` bits per second. grc's own setup supplied the
 * rest (HRD type 1 = VCL only, QP step limits 6 and 3). Bmin scales with the CPB size from grc's 277 at 5 Mbit
 * (unconfirmed: the header says the driver computes it at sequence level). */
void ApplyHwRc(u8 *setup, const HwRcConfig &c, u32 bitrate)
{
    auto *rc = reinterpret_cast<nvenc_h264_rc_s *>(setup + SetupRc);
    const u32 cpb = static_cast<u32>(static_cast<u64>(bitrate) * c.cpb_ms / 1000);
    rc->hrd_type = 1;
    rc->QP[0] = rc->QP[2] = static_cast<u8>(c.start_qp);
    rc->minQP[0] = rc->minQP[1] = rc->minQP[2] = static_cast<u8>(c.min_qp);
    rc->maxQP[0] = rc->maxQP[1] = rc->maxQP[2] = static_cast<u8>(c.max_qp);
    rc->framerate = static_cast<int>(c.fps << 8);
    rc->buffersize = cpb;
    rc->nal_cpb_size = 0;
    rc->nal_bitrate = 0;
    rc->vcl_cpb_size = static_cast<int>(cpb);
    rc->vcl_bitrate = static_cast<int>(bitrate);
    rc->gop_length = c.gop;
    rc->Np = static_cast<int>(c.gop - 1);
    rc->R = static_cast<int>(bitrate / (c.fps * ((NvfW / 16) * (NvfH / 16))));
    rc->Bmin = static_cast<int>(static_cast<u64>(277) * cpb / 5000000u);
}

/* One job, grc's shape, RCMODE 0. Returns 0 done, 1 rejected, 2 no
 * status within 1 s (the caller treats that as a stall). */
/* submit only. 0 submitted (fence and start time out), 1 rejected. */
int NvfSubmit(const NvfCtx &x, u32 pic_index, const NvfJob &j, u32 *out_fence, u64 *out_t0)
{
    u8 *const a = x.a;
    const NvfLayout &L = Layout720();
    /* poison only the picture index: it is what says "our job wrote this" */
    const u32 poison = 0xA5A5A5A5u;
    std::memcpy(a + L.off_status, std::addressof(poison), 4);
    armDCacheFlush(a + L.off_status, 0x40);

    auto *w = reinterpret_cast<u32 *>(a + L.off_cmd);
    u32 n = 0;
    auto m = [&](u32 method, u32 value) {
        w[n++] = vic::Host1xOpcodeIncr(vic::UCLASS_METHOD_OFFSET, 2);
        w[n++] = method >> 2;
        w[n++] = value;
    };
    w[n++] = vic::Host1xOpcodeSetClass(0, 0x21, 0);
    w[n++] = vic::Host1xOpcodeIncr(0x0B, 1);
    w[n++] = 0;
    m(0x700, j.control);                                                /* SET_CONTROL_PARAMS: H.264, RCMODE 0 or 18 */
    m(0x704, pic_index);                                                /* SET_PICTURE_INDEX         */
    m(0x200, 1);                                                        /* SET_APPLICATION_ID: H.264 */
    m(0x710, (x.enc + j.setup) >> 8);                                   /* SET_IN_DRV_PIC_SETUP      */
    m(0x718, (x.enc + L.off_status) >> 8);                              /* SET_OUT_ENC_STATUS        */
    m(0x724, (x.enc + L.off_rc) >> 8);                                  /* SET_IO_RC_PROCESS         */
    m(0x71C, (x.enc + L.off_bits) >> 8);                                /* SET_OUT_BITSTREAM         */
    m(0x720, (x.enc + L.off_hist) >> 8);                                /* SET_IOHISTORY             */
    m(0x734, (x.enc + (j.cur != 0 ? j.cur : L.off_cur)) >> 8);          /* SET_IN_CUR_PIC          */
    m(0x740, (x.enc + (j.cur_uv != 0 ? j.cur_uv : L.off_cur_uv)) >> 8); /* SET_IN_CUR_PIC_CHROMA_U */
    m(0x730, (x.enc + j.ref_out) >> 8);                                 /* SET_OUT_REF_PIC_LUMA      */
    if (j.ref_in != 0)
    {
        /* grc's P job, in grc's order (as recorded in its command buffers) */
        m(0x738, (x.enc + j.me_in) >> 8);  /* SET_IN_MEPRED_DATA        */
        m(0x73C, (x.enc + j.me_out) >> 8); /* SET_OUT_MEPRED_DATA       */
        m(0x400, (x.enc + j.ref_in) >> 8); /* SET_IN_REF_PIC0_LUMA      */
    }
    m(0x300, 0x100); /* EXECUTE                   */
    n = AppendIncrSyncpt(w, n, x.esyncpt, true);
    armDCacheFlush(w, 0x1000);

    alignas(8) u8 sb[16 + 12 + 20 + 4] = {};
    u32 off = 0;
    auto put = [&](u32 v) {
        std::memcpy(sb + off, std::addressof(v), 4);
        off += 4;
    };
    put(1);
    put(0);
    put(1);
    put(1);
    put(x.ah);
    put(L.off_cmd);
    put(n);
    put(x.esyncpt);
    put(1);
    put(0);
    put(0);
    put(0);
    const u32 fence_off = off;
    put(0);
    const u32 sz = off;
    const u32 req = (UINT32_C(3) << 30) | (sz << 16) | (0x00u << 8) | 0x01u;

    const u64 t0 = armTicksToNs(armGetSystemTick());
    u32 nverr = 0, fence_val = 0;
    const auto rc = NvIoctl(x.efd, req, sb, sz, std::addressof(nverr));
    std::memcpy(std::addressof(fence_val), sb + fence_off, 4);
    if (R_FAILED(rc) || nverr != 0)
    {
        return 1;
    }
    *out_fence = fence_val;
    *out_t0 = t0;
    return 0;
}

/* Wait for a submitted job: 0 done, 2 still incomplete after 1 s.
 * Check completion before the deadline: a descheduled waiter can wake
 * after the deadline even though NVENC completed while it slept. */
int NvfWaitDone(const NvfCtx &x, u32 pic_index, u32 fence_val, u64 t0, nvenc_pic_stat_s *st, u64 *enc_ns)
{
    u8 *const a = x.a;
    const NvfLayout &L = Layout720();
    const u64 w0 = armTicksToNs(armGetSystemTick());
    const u32 cfd = CtrlFd();
    bool fence_done = false, ours = false;
    auto sample_completion = [&]() {
        if (cfd != 0 && !fence_done)
        {
            struct
            {
                u32 id;
                u32 value;
            } r = {x.esyncpt, 0};
            u32 e2 = 0;
            const Result rc = NvIoctl(cfd, NvHostIocCtrlSyncptRead, std::addressof(r), sizeof(r), std::addressof(e2));
            if (R_SUCCEEDED(rc) && e2 == 0)
            {
                fence_done = (static_cast<s32>(r.value - fence_val) >= 0);
            }
        }
        armDCacheFlush(a + L.off_status, 0x80);
        std::memcpy(st, a + L.off_status, sizeof(*st));
        ours = (st->picture_index == pic_index);
    };
    for (;;)
    {
        sample_completion();
        if (fence_done && ours)
        {
            break;
        }
        if (armTicksToNs(armGetSystemTick()) - w0 >= UINT64_C(1000000000))
        {
            /* The thread may also be descheduled between the fence
             * read and the status read. Refresh both at the timeout
             * boundary instead of declaring a stale read a stall. */
            sample_completion();
            break;
        }
        os::SleepThread(TimeSpan::FromMicroSeconds(50));
    }
    *enc_ns = armTicksToNs(armGetSystemTick()) - t0;
    /* which half failed - the engine's syncpoint or its status write */
    g_nvf_last_fence_done = fence_done;
    g_nvf_last_ours = ours;
    g_nvf_last_fence = fence_val;
    g_nvf_last_pic = st->picture_index;
    return (fence_done && ours) ? 0 : 2;
}

/* has a submitted job finished? One syncpoint read, one status look,
 * no waiting - for use inside another wait. */

/* a 4-byte pixel's offset in any block-linear RGBA surface -
 * GOBs of 64 bytes x 8 rows (512 B), 2^bh_log2 GOBs to a block. MK8's
 * 1920-px stride with 16-GOB blocks was the only layout the size check
 * knew (Xenoblade Chronicles X back in handheld kept streaming 1080p). */
constexpr u64 BlOffset(u32 x, u32 y, u32 stride_px, u32 bh_log2)
{
    const u32 xb = x * 4;
    const u32 block_rows = 8u << bh_log2, block_bytes = 512u << bh_log2;
    const u64 gobs_x = (static_cast<u64>(stride_px) * 4 + 63) / 64;
    return static_cast<u64>(y / block_rows) * gobs_x * block_bytes + static_cast<u64>(xb / 64) * block_bytes +
           ((y % block_rows) / 8) * 512 + ((xb % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((xb % 32) / 16) * 32 +
           (y % 2) * 16 + (xb % 16);
}
/* the MK8 formula it replaces, at the probe's corners */
constexpr u64 Mk8Offset(u32 x, u32 y)
{
    const u32 xb = x * 4;
    return (y / 128) * 120 * 8192 + (xb / 64) * 8192 + ((y % 128) / 8) * 512 + ((xb % 64) / 32) * 256 +
           ((y % 8) / 2) * 64 + ((xb % 32) / 16) * 32 + (y % 2) * 16 + (xb % 16);
}
static_assert(BlOffset(1300, 10, 1920, 4) == Mk8Offset(1300, 10) &&
              BlOffset(1900, 1070, 1920, 4) == Mk8Offset(1900, 1070) &&
              BlOffset(10, 740, 1920, 4) == Mk8Offset(10, 740) && BlOffset(1279, 719, 1920, 4) == Mk8Offset(1279, 719));

/* All zero outside the top-left 1280x720 means handheld content. */
bool SlotContentIs720(const u8 *slot, u32 *out_samples, u32 stride_px, u32 bh_log2)
{
    auto px = [&](u32 x, u32 y) -> const u8 * { return slot + BlOffset(x, y, stride_px, bh_log2); };
    u32 n = 0;
    bool zero = true;
    for (u32 y = 10; y < 1080; y += 100)
    {
        for (u32 x = 1300; x < 1920; x += 100)
        {
            const u8 *p = px(x, y);
            zero &= (p[0] | p[1] | p[2]) == 0;
            ++n;
        }
    }
    for (u32 y = 740; y < 1080; y += 50)
    {
        for (u32 x = 10; x < 1280; x += 150)
        {
            const u8 *p = px(x, y);
            zero &= (p[0] | p[1] | p[2]) == 0;
            ++n;
        }
    }
    *out_samples = n;
    return zero;
}

/* what a 1080p slot holds, for the 720p/1080p decision. Smash
 * drawing 720p into the corner after docked mode leaves the docked
 * picture around it - not black, so "all black outside" kept the
 * stream at 1080p with the old frame framing the new one. So also:
 * a hash of the same 140 points outside the corner and of 64 inside,
 * compared from one check to the next - a 720p picture changes inside
 * only. */
struct ContentProbe
{
    bool ok, outside_black, inside_black;
    u32 outside_hash, inside_hash;
};

/* what the last size check decided, for the session it
 * restarts: its first frame cannot tell a 720p picture framed by an
 * old docked one from a 1080p picture (pid, buffer set, 1 = 720p) */
struct SizeHint
{
    u64 pid;
    u32 gen;
    s32 corner;
};
constinit SizeHint g_size_hint = {0, 0, -1};

ContentProbe ProbeContent(::ams::svc::Handle dbg, u64 base, u32 stride_px, u32 bh_log2)
{
    auto off = [&](u32 x, u32 y) -> u64 { return BlOffset(x, y, stride_px, bh_log2); };
    ContentProbe r = {true, true, true, 2166136261u, 2166136261u};
    alignas(8) u8 px[8];
    auto sample = [&](u32 x, u32 y, u32 *h, bool outside) {
        if (!r.ok)
        {
            return;
        }
        if (!DbgRead(dbg, px, base + off(x, y), 4))
        {
            r.ok = false;
            return;
        }
        if ((px[0] | px[1] | px[2]) != 0)
        {
            (outside ? r.outside_black : r.inside_black) = false;
        }
        for (u32 i = 0; i < 3; ++i)
        {
            *h = (*h ^ px[i]) * 16777619u;
        }
    };
    for (u32 y = 10; y < 1080; y += 100)
    {
        for (u32 x = 1300; x < 1920; x += 100)
        {
            sample(x, y, std::addressof(r.outside_hash), true);
        }
    }
    for (u32 y = 740; y < 1080; y += 50)
    {
        for (u32 x = 10; x < 1280; x += 150)
        {
            sample(x, y, std::addressof(r.outside_hash), true);
        }
    }
    for (u32 y = 30; y < 720; y += 90)
    {
        for (u32 x = 40; x < 1280; x += 160)
        {
            sample(x, y, std::addressof(r.inside_hash), false);
        }
    }
    return r;
}

// Recheck picture dimensions at a bounded interval to detect dock/undock or source changes.
struct SizeCheck
{
    bool started_corner = false;
    bool last_crop_full = false;
    u32 small_streak = 0, big_streak = 0, stale_streak = 0;
    ContentProbe last[8] = {}; /* per slot */
    u64 next_ns = 0;

    static bool CropFull(u32 crop)
    {
        return (crop >> 16) >= 1920 && (crop & 0xFFFF) >= 1080;
    }
    static bool CropSmall(u32 crop)
    {
        return crop != 0 && (crop >> 16) <= 1280 && (crop & 0xFFFF) <= 720;
    }

    explicit SizeCheck(bool corner)
        : started_corner(corner), last_crop_full(CropFull(g_queue_crop_wh.load(std::memory_order_relaxed))),
          next_ns(armTicksToNs(armGetSystemTick()) + UINT64_C(250000000))
    {
    }

    /* nullptr, or why the stream restarts at the other size (g_size_hint set) */
    const char *Step(::ams::svc::Handle dbg, u64 addr, s32 slot, u32 stride_px, u32 bh_log2)
    {
        const u64 now = armTicksToNs(armGetSystemTick());
        if (now < next_ns)
        {
            return nullptr;
        }
        next_ns = now + UINT64_C(250000000);
        const u32 crop = g_queue_crop_wh.load(std::memory_order_relaxed);
        const bool crop_full = CropFull(crop);
        const bool shrank = last_crop_full && CropSmall(crop);
        last_crop_full = crop_full;
        if (!started_corner && shrank)
        {
            return Switch(true, "the game switched to a 720p picture (its picture size went to 1280x720)");
        }
        const u64 pid = g_app_pid.load(std::memory_order_relaxed);
        if (pid != 0 && g_crop_full_pid.load(std::memory_order_relaxed) == pid)
        {
            if (!started_corner && CropSmall(crop))
            {
                return Switch(true, "the game switched to a 720p picture (its picture size is 1280x720)");
            }
            if (started_corner && crop_full)
            {
                return Switch(false, "the game switched to a 1080p picture (its picture size is 1920x1080)");
            }
            return nullptr;
        }
        if (g_queue_crop_xy.load(std::memory_order_relaxed) != 0)
        {
            /* an offset window: the corner and black-outside tests do not describe it, and the corner read misses it */
            return started_corner ? Switch(false, "the game presents a window away from the corner of its surface")
                                  : nullptr;
        }
        const ContentProbe pr = ProbeContent(dbg, addr, stride_px, bh_log2);
        ContentProbe &prev = last[static_cast<u32>(slot) & 7];
        s32 vote = -1; /* 1: 720p (black outside), 2: 720p (old picture outside), 0: 1080p, -1: no evidence */
        if (pr.ok)
        {
            const bool moved_in = prev.ok && pr.inside_hash != prev.inside_hash;
            const bool moved_out = prev.ok && pr.outside_hash != prev.outside_hash;
            if (crop_full)
            {
                vote = 0;
            }
            else if (pr.outside_black && pr.inside_black)
            {
                vote = -1;
            }
            else if (pr.outside_black)
            {
                vote = 1;
            }
            else if (moved_out)
            {
                vote = 0;
            }
            else if (moved_in)
            {
                vote = 2;
            }
        }
        prev = pr;
        /* 720p after 8 checks in a row (2 s), 1080p after 2 (0.5 s) */
        small_streak = (vote == 1 || vote == 2) ? small_streak + 1 : 0;
        stale_streak = vote == 2 ? stale_streak + 1 : 0;
        big_streak = vote == 0 ? big_streak + 1 : 0;
        if (!started_corner && small_streak >= 8)
        {
            return Switch(true,
                          stale_streak != 0
                              ? "the game switched to a 720p picture (an unchanging old picture outside the corner)"
                              : "the game switched to a 720p picture (black outside the corner)");
        }
        if (started_corner && big_streak >= 2)
        {
            return Switch(false, "the game switched to a 1080p picture");
        }
        return nullptr;
    }

    const char *Switch(bool to720, const char *why)
    {
        g_size_hint = {g_app_pid.load(std::memory_order_relaxed), g_game_surface.generation, to720 ? 1 : 0};
        return why;
    }
};

/* Everything one real-frame run needs, opened fresh and torn down in
 * reverse: the slot copy pinned for the VIC, the arena pinned for the
 * VIC and for a new msenc channel. A channel whose job stalled is left
 * OPEN (: tearing down a stuck channel took the compositor with it). */
/* the last per-slot buffer match (NvfSession::Calibrate): the next
 * session's first guess, so a restart (a size change, the viewer
 * reopened) starts on the right buffers */
struct SlotMatch
{
    u64 addr[8];
    u32 n;
    u32 buf;
};
constinit SlotMatch g_slot_match = {};

/* Everything one real-frame run needs, opened fresh and torn down in
 * reverse: the slot copy pinned for the VIC, the arena pinned for the
 * VIC and for a new msenc channel. A channel whose job stalled is left
 * OPEN (: tearing down a stuck channel took the compositor with it). */
struct NvfSession
{
    u32 vfd = 0, cmd_handle = 0, vsyncpt = 0, cfg_addr = 0;
    u32 sh = 0, slot_vic = 0, arena_vic = 0;
    NvfCtx x = {};
    bool efd_open = false, keep_open = false;
    SrcDesc src{1920, 1080, 1920, vic::BLK_KIND_GENERIC_16Bx2, 4, vic::PIXFMT_A8R8G8B8, vic::CACHE_WIDTH_64Bx4,
                1920, 1080};
    bool corner = false;  /* handheld: read and convert the 1280x720 corner 1:1 */
    u32 seen = 0;         /* g_queue_count at the last capture */
    u64 present_tick = 0; /* g_queue_tick of the present the last capture took */
    u32 present_flip = 0; /* its transform: bit 0 flip H, bit 1 flip V */
    /* waits on the present's acquire fence before reading */
    u32 fence_waits = 0, fence_timeouts = 0, fence_unknown = 0;
    u32 reread = 0; /* reads thrown away - the slot may have been reused mid-read */
    u64 fence_wait_sum = 0, fence_wait_max = 0;
    size_t arena_base = NvfArenaBase;
    u32 arena_size = NvfArenaSize;
    u32 nvfd = 0;          /* the nvmap fd our handles live on, to free them */
    /* the game's swapchain geometry. The defaults are MK8's, which
     * every run before  hard-coded: three 1920x1080 A8B8G8R8 slots
     * in one nvmap object, 128-row blocks. */
    u32 slot_off[8] = {0, 0x870000, 0x10E0000};
    u32 nslots = 3;
    u32 buf_bytes = static_cast<u32>(FbSlotSize);
    bool mk8_layout = true;    /* MK8's layout: the corner can be read alone (CornerBlockRows) */
    bool size_check_ok = true; /* any 1920x1080 surface - the 720p/1080p check runs */
    bool CornerFast() const
    {
        return corner && mk8_layout;
    }

    /* One nvmap object per slot (GameSurface::separate, Smash
     * Ultimate): every slot_off is 0, and slot_base + slot_off read
     * ONE buffer for every slot - each picture sent three times, 20
     * fps of picture in a 60 fps stream. And with no region exactly
     * the swapchain's size, slot_base is only the smallest region
     * that holds it - in Moonlight its texture memory, in DuckStation
     * nothing on screen. Then each slot reads its own buffer
     * instead: cand[] from FindSlotCandidates, slot_cand[k] the
     * candidate slot k reads. Until Calibrate has matched them, slot
     * k reads candidate k (one object per slot) or what slot_base +
     * slot_off[k] read before (one object), or the match the last
     * session found if its buffers are all still there. */
    static constexpr u32 MaxSlotCand = slotmatch::MaxCand;
    static constexpr u32 SigPoints = 32;
    bool sep_layout = false;    /* the geometry says one object per slot */
    bool sep = false;           /* reading per-slot buffers */
    bool sep_locked = false;    /* matched (or given up): no more sampling */
    bool still_guessed = false; /* slot_cand from StillGuess (a still picture), not yet matched */
    bool guess_trusted = false; /* slot_cand is the last session's match */
    u32 held_ms = 0;            /* how long the first frames waited for the match */
    bool hold_done = false;     /* the wait happens once a session */
    u64 cand[MaxSlotCand] = {};
    u32 ncand = 0;
    u8 slot_cand[8] = {};
    slotmatch::Matcher matcher;

    u64 SlotAddr(u64 slot_base, s32 slot) const
    {
        const bool valid = slot >= 0 && static_cast<u32>(slot) < nslots;
        if (sep)
        {
            return cand[slot_cand[valid ? slot : 0]];
        }
        return slot_base + (valid ? slot_off[slot] : 0);
    }

    /* after SetGeometry, while attached: the per-slot buffers -
     * unless slot_base is a region exactly the swapchain's size and
     * the slots are offsets in it (MK8) */
    void PrepareSlots(::ams::svc::Handle dbg, u64 slot_base, const char *who)
    {
        sep = sep_locked = still_guessed = false;
        u64 span = buf_bytes;
        for (u32 k = 0; k < nslots; ++k)
        {
            if (slot_off[k] + static_cast<u64>(buf_bytes) > span)
            {
                span = slot_off[k] + static_cast<u64>(buf_bytes);
            }
        }
        ::ams::svc::MemoryInfo mi = {};
        ::ams::svc::PageInfo pi = {};
        const bool exact = DbgQuery(dbg, std::addressof(mi), std::addressof(pi), slot_base) &&
                           mi.base_address == slot_base && mi.size == span;
        if (!sep_layout && exact)
        {
            return;
        }
        u32 devc[MaxSlotCand] = {};
        ncand = FindSlotCandidates(dbg, buf_bytes, slot_off, nslots, sep_layout, cand, MaxSlotCand, devc);
        /* one object holding the slots, in more than one region
         * that could hold it (Sonic: two pools laid out alike) - the
         * swapchain's region is also mapped for the display, so drop
         * the regions mapped for one device only. : keep every
         * region mapped for 2 or more, not only the most - Fire
         * Emblem: Three Houses has an unrelated region mapped for 3
         * and its swapchain's for 2; keeping only the 3 left nothing
         * to match (black menus, garbage). The matcher picks among
         * the rest. */
        if (!sep_layout && ncand > nslots)
        {
            u32 kept = 0;
            for (u32 c = 0; c < ncand; ++c)
            {
                if (devc[c] >= 2)
                {
                    ++kept;
                }
            }
            if (kept >= nslots && kept < ncand)
            {
                u32 k2 = 0;
                for (u32 c = 0; c < ncand; ++c)
                {
                    if (devc[c] >= 2)
                    {
                        cand[k2] = cand[c];
                        devc[k2] = devc[c];
                        ++k2;
                    }
                }
                LogLine(
                    "   %s: %u of %u candidates kept - the regions mapped for 2 or more devices (the display's too)",
                    who, kept, ncand);
                ncand = kept;
            }
        }
        if (ncand == MaxSlotCand)
        {
            LogLine("   %s: the candidate list is full (%u) - the slots may be past it", who, ncand);
        }
        if (ncand < nslots)
        {
            LogLine(
                "   %s: %u slot(s)%s, but %u candidate buffer(s) of %u B in the game's memory - reading from 0x%010llx as before",
                who, nslots, sep_layout ? ", each its own buffer" : "", ncand, buf_bytes,
                static_cast<unsigned long long>(slot_base));
            return;
        }
        for (u32 k = 0; k < nslots; ++k)
        {
            slot_cand[k] = static_cast<u8>(k);
            for (u32 c = 0; c < ncand && !sep_layout; ++c)
            {
                if (cand[c] == slot_base + slot_off[k])
                {
                    slot_cand[k] = static_cast<u8>(c);
                    break;
                }
            }
        }
        bool reused = false;
        if (g_slot_match.n == nslots && g_slot_match.buf == buf_bytes)
        {
            u8 m[8] = {};
            u32 hits = 0;
            for (u32 k = 0; k < nslots; ++k)
            {
                for (u32 c = 0; c < ncand; ++c)
                {
                    if (cand[c] == g_slot_match.addr[k])
                    {
                        m[k] = static_cast<u8>(c);
                        ++hits;
                        break;
                    }
                }
            }
            if (hits == nslots)
            {
                std::memcpy(slot_cand, m, sizeof(slot_cand));
                reused = true;
            }
            guess_trusted = reused;
        }
        matcher.Reset(nslots, ncand);
        sep = true;
        LogLine(
            "   %s: %u slot(s), %s; %u candidate buffer(s) of %u B from 0x%010llx%s - matching them to slots while streaming",
            who, nslots, sep_layout ? "each its own nvmap object" : "no region exactly the swapchain's size", ncand,
            buf_bytes, static_cast<unsigned long long>(cand[0]),
            reused ? " (starting from the last session's match)" : "");
    }

    /* a hash of SigPoints 64-byte samples spread over the buffer;
     * *varied false when every sample is one repeated 4-byte value
     * (a cleared buffer) */
    u32 SampleSig(::ams::svc::Handle dbg, u64 base, bool *varied) const
    {
        const u64 step = (buf_bytes / SigPoints) & ~UINT64_C(63);
        u32 h = 2166136261u, first = 0;
        bool var = false;
        for (u32 j = 0; j < SigPoints; ++j)
        {
            alignas(64) u8 b[64];
            if (!DbgRead(dbg, b, base + j * step + ((step / 2) & ~UINT64_C(63)), sizeof(b)))
            {
                *varied = false;
                return 0;
            }
            if (j == 0)
            {
                std::memcpy(std::addressof(first), b, sizeof(first));
            }
            for (u32 i = 0; i < sizeof(b); i += 4)
            {
                var |= std::memcmp(b + i, std::addressof(first), 4) != 0;
            }
            for (u32 i = 0; i < sizeof(b); ++i)
            {
                h = (h ^ b[i]) * 16777619u;
            }
        }
        *varied = var;
        return h;
    }

    /* once the present of `slot` (present number `count`) is
     * finished, before it is read: a sample for the slot matcher
     * (applet_mitm_slotmatch.hpp) until it has matched every slot */
    void Calibrate(::ams::svc::Handle dbg, u32 count, s32 slot)
    {
        if (!sep || sep_locked || slot < 0 || static_cast<u32>(slot) >= nslots)
        {
            return;
        }
        u32 sig[MaxSlotCand];
        bool varied[MaxSlotCand];
        for (u32 c = 0; c < ncand; ++c)
        {
            sig[c] = this->SampleSig(dbg, cand[c], std::addressof(varied[c]));
        }
        /* the slots drawn since the last sample, from the present
         * ring - a stream slower than the game skips presents */
        u32 drawn = 0;
        if (matcher.have && count > matcher.last && count - matcher.last <= PresentRingSize - 2)
        {
            for (u32 c = matcher.last + 1; c <= count; ++c)
            {
                PresentSnap p;
                if (c == count)
                {
                    drawn |= 1u << slot;
                    continue;
                }
                if (!ReadPresent(c, std::addressof(p)) || p.slot < 0 || static_cast<u32>(p.slot) >= nslots)
                {
                    drawn = 0;
                    break;
                }
                drawn |= 1u << p.slot;
            }
        }
        matcher.AddSpan(count, drawn, sig);
        u8 m[8] = {};
        s32 weakest = 0;
        if (matcher.Match(m, std::addressof(weakest)))
        {
            std::memcpy(slot_cand, m, nslots);
            sep_locked = true;
            g_slot_match.n = nslots;
            g_slot_match.buf = buf_bytes;
            char line[200];
            int len = std::snprintf(
                line, sizeof(line),
                "   swapchain: slots matched to their buffers after %u frame intervals (weakest score %d/1000):",
                matcher.intervals, weakest);
            for (u32 s = 0; s < nslots && len > 0 && static_cast<size_t>(len) < sizeof(line); ++s)
            {
                g_slot_match.addr[s] = cand[m[s]];
                len += std::snprintf(line + len, sizeof(line) - len, " %u->0x%010llx", s,
                                     static_cast<unsigned long long>(cand[m[s]]));
            }
            LogLine("%s", line);
        }
        else if (u8 g[8] = {}; !still_guessed && slotmatch::StillGuess(sig, varied, ncand, nslots, !sep_layout, g))
        {
            /* the picture stands still: show it while waiting for motion */
            std::memcpy(slot_cand, g, nslots);
            still_guessed = true;
            LogLine(
                "   swapchain: the picture stands still - slot 0 shows 0x%010llx (the same picture in every slot) until motion tells them apart",
                static_cast<unsigned long long>(cand[g[0]]));
        }
        else if (matcher.GaveUp())
        {
            sep_locked = true;
            LogLine("   swapchain: slots not told apart in %u samples (%u with a change) - keeping the first guess",
                    matcher.samples, matcher.intervals);
        }
    }

    bool composed_source = false;
    void SetComposed()
    {
        composed_source = true;
        src = SrcDesc{1280, 720, 1280, vic::BLK_KIND_PITCH, 0, vic::PIXFMT_A8R8G8B8, vic::CACHE_WIDTH_64Bx4, 1280, 720};
        buf_bytes = 1280 * 720 * 4;
        nslots = 1;
        slot_off[0] = 0;
        mk8_layout = size_check_ok = corner = false;
        cap_crop = (1280u << 16) | 720u;
    }

    bool SetGeometry(const GameSurface &g, const char *who)
    {
        if (g.num_slots == 0 || g.buf_size == 0 || g.width == 0 || g.height == 0)
        {
            LogLine("   %s: no buffer geometry recorded for this game", who);
            return false;
        }
        if (g.layout != 3)
        {
            LogLine("   %s: swapchain layout %u is not block-linear - not supported", who, g.layout);
            return false;
        }
        /* up to the 1080p slot plus the spare stage after it in
         * g_ind_buf (32-row blocks make a 1080p buffer 9.8 MB) */
        if (g.buf_size > MaxSlotBytes || g.width > 1920 || g.height > 1088)
        {
            LogLine("   %s: buffer %ux%u, %u B - larger than %zu B, not supported", who, g.width, g.height, g.buf_size,
                    MaxSlotBytes);
            return false;
        }
        for (u32 k = 0; k < 8; ++k)
        {
            slot_off[k] = g.slot_offset[k];
        }
        nslots = g.num_slots;
        /* one nvmap object per slot (Smash, Minecraft): their ids differ */
        sep_layout = false;
        for (u32 k = 1; k < g.num_slots && k < 8; ++k)
        {
            if (g.slot_nvmap[k] != g.slot_nvmap[0])
            {
                sep_layout = true;
            }
        }
        buf_bytes = g.buf_size;
        /* MK8's A8B8G8R8 surface reads correctly as the VIC's
         * A8R8G8B8 (/: R,G,B,A in memory); the same is assumed
         * for any other format and said in the log */
        /* NvColorFormat A8R8G8B8 (0x100d12120) is B, G, R, A in
         * memory - the VIC's A8B8G8R8, the other way round */
        const bool bgra = g.color_format == 0x100d12120ull;
        const bool known_fmt = g.color_format == 0x100532120ull || bgra;
        src = SrcDesc{g.width,
                      g.height,
                      g.stride_px,
                      vic::BLK_KIND_GENERIC_16Bx2,
                      g.block_h_log2,
                      bgra ? vic::PIXFMT_A8B8G8R8 : vic::PIXFMT_A8R8G8B8,
                      vic::CACHE_WIDTH_64Bx4,
                      g.width,
                      g.height};
        mk8_layout = g.width == 1920 && g.height == 1080 && g.stride_px == 1920 && g.block_h_log2 == 4;
        size_check_ok = g.width == 1920 && g.height == 1080 && g.stride_px >= 1920 && g.block_h_log2 <= 5;
        LogLine("   %s: game swapchain %ux%u stride %u px, block height 2^%u, %u slot(s) of %u B, format %#llx%s", who,
                g.width, g.height, g.stride_px, g.block_h_log2, nslots, buf_bytes,
                static_cast<unsigned long long>(g.color_format), known_fmt ? "" : " (not MK8's - colours unverified)");
        return true;
    }

    ~NvfSession()
    {
        if (efd_open && !keep_open)
        {
            if (x.enc != 0)
            {
                UnmapCmdBuffer(x.efd, x.ah);
            }
            NvClose(x.efd);
        }
        if (arena_vic != 0)
        {
            UnmapCmdBuffer(vfd, x.ah);
        }
        if (slot_vic != 0)
        {
            UnmapCmdBuffer(vfd, sh);
        }
        /* unmapped everywhere - free the handles, so the
         * capture region can go back to the system. Not when a job
         * may still be in the channel (keep_open): the arena stays
         * referenced and CaptureRelease keeps the memory. */
        if (!keep_open && nvfd != 0)
        {
            if (x.ah != 0 && NvmapFree(nvfd, x.ah))
            {
                --g_capture_handles;
            }
            if (sh != 0 && NvmapFree(nvfd, sh))
            {
                --g_capture_handles;
            }
        }
    }

    bool Open(u32 v, u32 nvmap_fd, u32 cmd, u32 vsp, u32 cfg, const char *who)
    {
        vfd = v;
        cmd_handle = cmd;
        vsyncpt = vsp;
        cfg_addr = cfg;
        if (g_engine_wedged)
        {
            LogLine("   %s: an earlier engine job never completed - not submitting", who);
            return false;
        }
        if (g_ind_buf == nullptr || g_ind_size < arena_base + arena_size || cfg_addr == 0)
        {
            LogLine("   %s: capture region %zu KB, need %zu KB (cfg %#x) - not running", who, g_ind_size / 1024,
                    (arena_base + arena_size) / 1024, cfg_addr);
            return false;
        }
        u32 si = 0;
        /* the slot copy, as the VIC sees it: one 1080p slot, or more for a
         * larger buffer */
        const u32 slot_map = static_cast<u32>(buf_bytes > FbSlotSize ? ((buf_bytes + 0xFFFu) & ~0xFFFu) : FbSlotSize);
        if (R_FAILED(NvmapOwn(nvmap_fd, g_ind_buf, slot_map, 0, std::addressof(sh), std::addressof(si), true)))
        {
            LogLine("   %s: nvmap of the slot copy failed", who);
            return false;
        }
        nvfd = nvmap_fd;
        ++g_capture_handles;
        HoldDeviceZero(vfd); /* .1 */
        MapCmdBuffer(vfd, sh, std::addressof(slot_vic), "nvf-slot", 0);
        if (slot_vic == 0)
        {
            LogLine("   %s: slot pin returned 0 - refusing", who);
            return false;
        }

        /* zeroed and flushed BEFORE any engine writes it, so no dirty
         * line from the memset can be written back over engine output */
        x.a = g_ind_buf + arena_base;
        std::memset(x.a, 0, arena_size);

        std::memcpy(x.a + Layout720().off_setup, Layout720().idr_setup, 0x1000);

        armDCacheFlush(x.a, arena_size);
        u32 aid = 0;
        if (R_FAILED(NvmapOwn(nvmap_fd, x.a, arena_size, 0, std::addressof(x.ah), std::addressof(aid), true)))
        {
            LogLine("   %s: nvmap of the %u KB arena failed", who, arena_size / 1024);
            return false;
        }
        ++g_capture_handles;
        MapCmdBuffer(vfd, x.ah, std::addressof(arena_vic), "nvf-arena(vic)", 0);
        if (arena_vic == 0)
        {
            LogLine("   %s: arena pin (VIC) returned 0 - refusing", who);
            return false;
        }

        u32 nverr = 0;
        if (R_FAILED(NvOpen("/dev/nvhost-msenc", std::addressof(x.efd), std::addressof(nverr))) || nverr != 0)
        {
            LogLine("   %s: /dev/nvhost-msenc open FAILED nverr=%u", who, nverr);
            return false;
        }
        efd_open = true;
        {
            struct
            {
                u32 module_id;
                u32 syncpt;
            } gs = {0, 0};
            nverr = 0;
            NvIoctl(x.efd, NvHostIocChannelGetSyncpoint, std::addressof(gs), sizeof(gs), std::addressof(nverr));
            x.esyncpt = gs.syncpt;
        }
        if (x.esyncpt == 0 || x.esyncpt == 12)
        {
            LogLine("   %s: msenc syncpt=%u - REFUSING", who, x.esyncpt);
            return false;
        }
        {
            struct
            {
                u32 fd;
            } sn = {nvmap_fd};
            nverr = 0;
            NvIoctl(x.efd, NvHostIocChannelSetNvmapFd, std::addressof(sn), sizeof(sn), std::addressof(nverr));
        }
        MapCmdBuffer(x.efd, x.ah, std::addressof(x.enc), "nvf-arena(enc)", 0);
        if (x.enc == 0)
        {
            LogLine("   %s: arena pin (NVENC) returned 0 - refusing", who);
            return false;
        }
        LogLine(
            "   %s: arena %u KB at +%zu KB; VIC sees it at %#x, NVENC at %#x (syncpt %u); picture %#x/%#x, bits %#x",
            who, arena_size / 1024, arena_base / 1024, arena_vic, x.enc, x.esyncpt, x.enc + Layout720().off_cur,
            x.enc + Layout720().off_cur_uv, x.enc + Layout720().off_bits);
        seen = g_queue_count.load(std::memory_order_relaxed);
        return true;
    }

    // Wait for the producer fence before copying a queued slot.

    u64 last_flush_ns = 0;

    /* was 30 ms. Pokemon Sword queues each frame ~20 ms
     * before its GPU work ends (SooraMaru's log: avg 18 ms, max 68,
     * 136 timeouts in 15 minutes), and a read past the timeout took
     * a picture the GPU was still drawing - the dot-grid frames in
     * fades and menu transitions. The GPU finishes in order, so a
     * longer wait only delays the stream; it never deadlocks it. */
    static constexpr u64 FenceTimeoutNs = UINT64_C(250000000);

    void WaitPresentFence(u32 n, const u64 fence[4])
    {
        if (n == 0 || n > 4)
        {
            ++fence_unknown;
            return;
        }
        const u32 cfd = CtrlFd();
        if (cfd == 0)
        {
            ++fence_unknown;
            return;
        }
        const u64 t0 = armTicksToNs(armGetSystemTick());
        bool timed_out = false;
        for (u32 k = 0; k < n; ++k)
        {
            const u64 f = fence[k];
            const u32 id = static_cast<u32>(f >> 32), want = static_cast<u32>(f);
            if (id == 0xFFFFFFFFu || id >= 192)
            {
                continue;
            } /* an unused entry */
            for (;;)
            {
                struct
                {
                    u32 id;
                    u32 value;
                } r = {id, 0};
                u32 e = 0;
                if (R_FAILED(NvIoctl(cfd, NvHostIocCtrlSyncptRead, std::addressof(r), sizeof(r), std::addressof(e))) ||
                    e != 0)
                {
                    break;
                }
                if (static_cast<s32>(r.value - want) >= 0)
                {
                    break;
                }
                if (armTicksToNs(armGetSystemTick()) - t0 > FenceTimeoutNs)
                {
                    timed_out = true;
                    break;
                }
                os::SleepThread(TimeSpan::FromMicroSeconds(250));
            }
        }
        const u64 w = armTicksToNs(armGetSystemTick()) - t0;
        ++fence_waits;
        fence_wait_sum += w;
        if (w > fence_wait_max)
        {
            fence_wait_max = w;
        }
        if (timed_out)
        {
            ++fence_timeouts;
        }
    }

    /* 1 = every syncpoint reached, 0 = the GPU is still drawing,
     * -1 = no readable fence (treated as done, as  did) */
    static int FenceState(u32 n, const u64 fence[4])
    {
        if (n == 0 || n > 4)
        {
            return -1;
        }
        const u32 cfd = CtrlFd();
        if (cfd == 0)
        {
            return -1;
        }
        for (u32 k = 0; k < n; ++k)
        {
            const u32 id = static_cast<u32>(fence[k] >> 32), want = static_cast<u32>(fence[k]);
            if (id == 0xFFFFFFFFu || id >= 192)
            {
                continue;
            }
            struct
            {
                u32 id;
                u32 value;
            } r = {id, 0};
            u32 e = 0;
            if (R_FAILED(NvIoctl(cfd, NvHostIocCtrlSyncptRead, std::addressof(r), sizeof(r), std::addressof(e))) ||
                e != 0)
            {
                return -1;
            }
            if (static_cast<s32>(r.value - want) < 0)
            {
                return 0;
            }
        }
        return 1;
    }

    struct PresentSnap
    {
        u32 count;
        s32 slot;
        u32 fence_n;
        u64 fence[4];
        u64 tick;
        u32 transform;
        u32 crop;
        u32 win;
        u32 win_xy;
    };

    /* present c from the ring; false if it was overwritten (8+ behind) */
    static bool ReadPresent(u32 c, PresentSnap *out)
    {
        PresentRec &r = g_present_ring[c % PresentRingSize];
        for (u32 tries = 0; tries < 8; ++tries)
        {
            const u32 s1 = r.seq.load(std::memory_order_acquire);
            if (s1 & 1)
            {
                continue;
            }
            out->count = r.count.load(std::memory_order_relaxed);
            out->slot = r.slot.load(std::memory_order_relaxed);
            out->fence_n = r.fence_n.load(std::memory_order_relaxed);
            for (u32 k = 0; k < 4; ++k)
            {
                out->fence[k] = r.fence[k].load(std::memory_order_relaxed);
            }
            out->tick = r.tick.load(std::memory_order_relaxed);
            out->transform = r.transform.load(std::memory_order_relaxed);
            out->crop = r.crop.load(std::memory_order_relaxed);
            out->win = r.win.load(std::memory_order_relaxed);
            out->win_xy = r.win_xy.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (r.seq.load(std::memory_order_relaxed) == s1)
            {
                return out->count == c;
            }
        }
        return false;
    }

    /* how each capture chose its present */
    u32 pick_ready = 0, pick_waited = 0, pick_legacy = 0;

    /* read in 2 MB pieces. One 8 MB ReadDebugProcessMemory holds
     * the game's page-table lock for the whole ~8.5 ms copy
     * (KPageTableBase::ReadDebugMemory); pieces let the game in
     * between. A few extra SVCs, microseconds. */
    static constexpr u32 ReadPiece = 0x200000;

    // Follow the latest queued slot; wait for its fence before reading the pixels.
    bool SlotMayBeReused(u32 c) const
    {
        PresentSnap next;
        if (!ReadPresent(c + 1, std::addressof(next)))
        {
            return false;
        }
        if (FenceState(next.fence_n, next.fence) != 1)
        {
            return false;
        }
        return nslots <= 2 || g_queue_count.load(std::memory_order_acquire) - c >= nslots - 1;
    }

    /* before the first frame of a buffer set the matcher has
     * not seen, watch presents go by - every one, in order, once its
     * fence is reached - until it matches or sees a still picture, for
     * up to 2.5 s. Sonic Frontiers keeps both slots 32 MB into one
     * large object: the first guess was another region and the first
     * ~0.75 s of the stream (22 frames at 30 fps) was noise. */
    void HoldForMatch(::ams::svc::Handle dbg)
    {
        if (hold_done || !sep || sep_locked || still_guessed || guess_trusted)
        {
            return;
        }
        hold_done = true;
        const u64 t0 = armTicksToNs(armGetSystemTick());
        u32 c = g_queue_count.load(std::memory_order_acquire);
        while (!sep_locked && !still_guessed && armTicksToNs(armGetSystemTick()) - t0 < UINT64_C(2500000000))
        {
            if (g_queue_count.load(std::memory_order_acquire) == c)
            {
                os::SleepThread(TimeSpan::FromMilliSeconds(1));
                continue;
            }
            ++c;
            PresentSnap p;
            if (!ReadPresent(c, std::addressof(p)))
            {
                c = g_queue_count.load(std::memory_order_acquire);
                continue;
            } /* fell behind the ring */
            for (u32 w = 0; w < 100 && FenceState(p.fence_n, p.fence) == 0; ++w)
            {
                os::SleepThread(TimeSpan::FromMicroSeconds(500));
            }
            this->Calibrate(dbg, p.count, p.slot);
        }
        held_ms = static_cast<u32>((armTicksToNs(armGetSystemTick()) - t0) / 1000000);
        LogLine("   swapchain: the first frame waited %u ms for the slot match (%s)", held_ms,
                sep_locked      ? "matched"
                : still_guessed ? "a still picture"
                                : "not yet - streaming the first guess");
        seen = g_queue_count.load(std::memory_order_acquire) - 1; /* the latest present is new */
    }

    bool Capture(::ams::svc::Handle dbg, u64 slot_base, u64 *read_ns, u32 *sig, u32 *dropped = nullptr)
    {
        if (composed_source)
        {
            /* with no application running, assemble the frame from vi's and overlayDisp's layers; any
             * failure is counted and logged and this frame comes from the screenshot source as before */
            bool layers_ok = false;
            if (UnifiedLayersEligible())
            {
                layers_ok = UnifiedLayersReadFrame(g_ind_buf, g_ind_size, read_ns, &present_tick);
                if (!layers_ok)
                {
                    UnifiedLayersNoteFallback();
                }
            }
            if (!layers_ok && !NxrpReadFrame(g_ind_buf, g_ind_size, read_ns, &present_tick))
                return false;

            present_flip = 0;
            last_flush_ns = 0;
            last_read_bytes = 1280 * 720 * 4;
            *sig = 0;
            return true;
        }

        this->HoldForMatch(dbg);
        for (u32 spins = 0; g_queue_count.load(std::memory_order_acquire) == seen && spins < 50; ++spins)
        {
            os::SleepThread(TimeSpan::FromMilliSeconds(1));
        }
        // a low-present-rate game must not repeatedly encode an old
        // slot. Wait for an actual new present while collecting the prior
        // encode; abandon promptly on viewer/focus loss or an idle game.
        {
            while (g_queue_count.load(std::memory_order_acquire) == seen)
            {
                const u64 qt = g_queue_tick.load(std::memory_order_relaxed);
                if (!CaptureVideoViewerPresent() || !HybridGameFocused() ||
                    (qt && armTicksToNs(armGetSystemTick() - qt) >= 1000000000ULL))
                    return false;
                os::SleepThread(TimeSpan::FromMilliSeconds(1));
            }
        }
        for (u32 retry = 0;;)
        {

            const u32 qnow = g_queue_count.load(std::memory_order_acquire);
            if (qnow == seen)
            {
                break;
            }
            {
                const u32 back = qnow - seen;
                const u32 lo =
                    back > PresentRingSize - 2 ? qnow - (PresentRingSize - 2) : seen; /* candidates (lo, qnow] */
                PresentSnap pick = {}, oldest = {};
                bool have_pick = false, have_oldest = false;
                for (u32 c = qnow; c != lo; --c)
                {
                    PresentSnap p;
                    if (!ReadPresent(c, std::addressof(p)))
                    {
                        continue;
                    }
                    if (FenceState(p.fence_n, p.fence) != 0)
                    {
                        pick = p;
                        have_pick = true;
                        break;
                    }
                    oldest = p;
                    have_oldest = true;
                }
                if (!have_pick && have_oldest)
                {
                    this->WaitPresentFence(oldest.fence_n, oldest.fence);
                    pick = oldest;
                    have_pick = true;
                    ++pick_waited;
                }
                else if (have_pick)
                {
                    ++fence_waits; /* a wait of 0, so the average stays per capture */
                    ++pick_ready;
                }
                if (have_pick)
                {
                    if (dropped != nullptr && pick.count - seen > 1)
                    {
                        *dropped += pick.count - seen - 1;
                    }
                    seen = pick.count;
                    present_tick = pick.tick;
                    present_flip = pick.transform & 3;
                    cap_crop = pick.crop;
                    cap_win = pick.win;
                    cap_win_xy = pick.win_xy;
                    const s32 slot = pick.slot;
                    this->Calibrate(dbg, pick.count, slot);
                    if (!this->ReadSlot(dbg, this->SlotAddr(slot_base, slot), read_ns, sig))
                    {
                        return false;
                    }
                    /* the slot is the game's again once the present
                     * after it is on screen - and that needs its fence. If
                     * it was reached before the read ended (a slow read, a
                     * double-buffered game), the read may hold some of the
                     * NEXT picture: take that newer one instead. Triple
                     * buffering needs two presents queued after ours. */
                    if (retry < 2 && SlotMayBeReused(pick.count) && ChangedSinceRead(dbg))
                    {
                        ++reread;
                        ++retry;
                        continue;
                    }
                    return true;
                }
            }
            break;
        }
        ++pick_legacy;
        // Capture the slot identified by the same presentation record used for the fence.
        u32 now = 0, fn = 0;
        u64 fence[4] = {};
        s32 slot = -1;
        for (u32 tries = 0; tries < 8; ++tries)
        {
            const u32 s1 = g_queue_seq.load(std::memory_order_acquire);
            if (s1 & 1)
            {
                continue;
            }
            now = g_queue_count.load(std::memory_order_acquire);
            slot = g_queue_slot.load(std::memory_order_relaxed);
            fn = g_queue_fence_n.load(std::memory_order_relaxed);
            for (u32 k = 0; k < 4; ++k)
            {
                fence[k] = g_queue_fence[k].load(std::memory_order_relaxed);
            }
            present_tick = g_queue_tick.load(std::memory_order_relaxed);
            present_flip = g_queue_transform.load(std::memory_order_relaxed) & 3;
            cap_crop = g_queue_crop_origin0.load(std::memory_order_relaxed)
                           ? g_queue_crop_wh.load(std::memory_order_relaxed)
                           : 0;
            cap_win = g_queue_crop_origin0.load(std::memory_order_relaxed) ? 0
                                                                           : g_queue_crop_wh.load(std::memory_order_relaxed);
            cap_win_xy = g_queue_crop_xy.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (g_queue_seq.load(std::memory_order_relaxed) == s1)
            {
                break;
            }
        }
        if (now != seen)
        {
            this->WaitPresentFence(fn, fence);
        }
        if (dropped != nullptr && now - seen > 1)
        {
            *dropped += now - seen - 1;
        }
        seen = now;
        this->Calibrate(dbg, now, slot);
        return this->ReadSlot(dbg, this->SlotAddr(slot_base, slot), read_ns, sig);
    }

    /* from WillMidia's PR #4: did the slot change after it
     * was copied? Eight 64-byte samples of the slot against the copy -
     * the cheap confirmation before a re-read (SlotMayBeReused only
     * knows the slot MAY be the game's again: Smash's menus re-read
     * 78 frames in one session, each a full copy). */
    u64 last_read = 0;

    bool ChangedSinceRead(::ams::svc::Handle dbg) const
    {
        for (u32 k = 0; k < 8; ++k)
        {
            const u64 off = CornerFast() ? static_cast<u64>(k % CornerBlockRows) * FbBlockRow +
                                               ((static_cast<u64>(k) * 0x2340u % CornerBlockRowBytes) & ~UINT64_C(63))
                                         : ((static_cast<u64>(last_read_bytes) / 8 * k + 1024) & ~UINT64_C(63));
            alignas(64) u8 b[64];
            if (!DbgRead(dbg, b, last_read + off, sizeof(b)))
            {
                return false;
            }
            if (std::memcmp(b, g_ind_buf + off, sizeof(b)) != 0)
            {
                return true;
            }
        }
        return false;
    }

    /* a surface other than 1920x1080 is read only down to the
     * last block row holding what the game shows - SMO handheld: 6 of
     * its 8 (768 of 900 rows), a quarter less to copy. The rows below
     * stay stale in g_ind_buf; the VIC never samples them. */
    u32 ReadBytes() const
    {
        if (size_check_ok)
        {
            return buf_bytes;
        }
        const u32 rows = PresentRect(src, cap_crop) & 0xFFFF;
        const u32 blk_rows = 8u << src.blk_h_log2;
        const u32 row_bytes = src.stride_px * 4;
        const u64 blk_bytes = static_cast<u64>(row_bytes) * blk_rows;
        if (row_bytes % 64 != 0 || blk_bytes == 0 || buf_bytes % blk_bytes != 0)
        {
            return buf_bytes;
        }
        const u64 n = (static_cast<u64>(rows) + blk_rows - 1) / blk_rows * blk_bytes;
        return n < buf_bytes ? static_cast<u32>(n) : buf_bytes;
    }

    bool ReadSlot(::ams::svc::Handle dbg, u64 base, u64 *read_ns, u32 *sig)
    {
        const u64 t0 = armTicksToNs(armGetSystemTick());
        last_read = base;
        if (CornerFast())
        {
            for (u32 br = 0; br < CornerBlockRows; ++br)
            {
                u8 *dst = g_ind_buf + br * FbBlockRow;
                if (!DbgRead(dbg, dst, base + br * FbBlockRow, CornerBlockRowBytes))
                {
                    return false;
                }
                armDCacheFlush(dst, CornerBlockRowBytes);
            }
        }
        else
        {
            const u32 bytes = ReadBytes();
            last_read_bytes = bytes;
            for (u32 off = 0; off < bytes; off += ReadPiece)
            {
                const u32 n = bytes - off < ReadPiece ? bytes - off : ReadPiece;
                if (!DbgRead(dbg, g_ind_buf + off, base + off, n))
                {
                    return false;
                }
            }
            const u64 tf = armTicksToNs(armGetSystemTick());
            armDCacheFlush(g_ind_buf, bytes);
            last_flush_ns = armTicksToNs(armGetSystemTick()) - tf;
        }
        u32 h = 0;
        for (u32 k = 0; k < 8192; k += 8)
        {
            h = h * 31u + g_ind_buf[k];
        }
        *sig = h;
        *read_ns = armTicksToNs(armGetSystemTick()) - t0;
        return true;
    }

    /* after the first full capture: handheld content -> corner mode */
    void Decide(const char *who)
    {
        if (!size_check_ok)
        {
            corner = false;
            LogLine("   %s: the VIC converts the whole %ux%u picture to 1280x720", who, src.w, src.h);
            return;
        }
        u32 samples = 0;
        corner = SlotContentIs720(g_ind_buf, std::addressof(samples), src.stride_px, src.blk_h_log2);
        /* a size check just restarted the stream - its finding
         * stands (the first frame alone cannot see old docked content) */
        if (g_size_hint.corner >= 0 && g_size_hint.pid == g_app_pid.load(std::memory_order_relaxed) &&
            g_size_hint.gen == g_game_surface.generation)
        {
            if ((g_size_hint.corner != 0) != corner)
            {
                LogLine("   %s: the last check found a %s picture - starting at that size", who,
                        g_size_hint.corner ? "720p" : "1080p");
            }
            corner = g_size_hint.corner != 0;
        }
        /* a game whose crop follows its mode - its crop decides */
        {
            const u64 pid = g_app_pid.load(std::memory_order_relaxed);
            const u32 crop = g_queue_crop_wh.load(std::memory_order_relaxed);
            if (pid != 0 && g_crop_full_pid.load(std::memory_order_relaxed) == pid && crop != 0)
            {
                const bool small = (crop >> 16) <= 1280 && (crop & 0xFFFF) <= 720;
                if (small != corner)
                {
                    LogLine("   %s: the game's picture size is %ux%u - starting at that size", who, crop >> 16,
                            crop & 0xFFFF);
                }
                corner = small;
            }
        }
        {
            u32 wx = 0, wy = 0, ww = 0, wh = 0;
            if (OffsetWindow(src, &wx, &wy, &ww, &wh))
            {
                if (corner)
                {
                    LogLine("   %s: the game presents %ux%u at (%u,%u) of its surface - the VIC converts that window", who,
                            ww, wh, wx, wy);
                }
                corner = false;
            }
        }
        if (corner)
        {
            src.rect_w = NvfW;
            src.rect_h = NvfH;
        }
        LogLine("   %s: content %s (%u samples outside the top-left 1280x720 %s) -> %s", who,
                corner ? "1280x720 (handheld)" : "1920x1080 (docked)", samples, corner ? "all zero" : "not all zero",
                corner ? (mk8_layout ? "the VIC converts the corner 1:1; later frames read only its 3.9 MB"
                                     : "the VIC converts the corner 1:1")
                       : "the VIC scales it to 1280x720");
    }

    /* a surface other than 1920x1080 - the VIC samples only
     * what the game presents. Super Mario Odyssey draws into 1600x900
     * and presents its top-left 1280x720 in handheld (the whole 1600x900
     * docked); scaling the whole surface left the picture in the top-left
     * 80% of the stream (creeper3010, #7). Followed on every frame, so a
     * docked/handheld switch follows too. Only a 16:9 crop from (0,0)
     * that fits the surface: 1920x1080 surfaces keep the size check
     * (Smash presents 1280x720 while drawing 1920x1080). */
    u32 crop_used = 0;
    u32 cap_crop = 0;        /* the captured present's crop (PresentRec::crop) */
    u32 cap_win = 0;         /* its crop when it does not start at (0,0): w << 16 | h (PresentRec::win) */
    u32 cap_win_xy = 0;      /* and that crop's left << 16 | top */
    u32 last_read_bytes = 0; /* how much of the slot ReadSlot copied */
    /* crop: the captured present's (cap_crop) */
    static u32 PresentRect(const SrcDesc &d, u32 crop)
    {
        const u32 cw = crop >> 16, ch = crop & 0xFFFF;
        const bool ok = crop != 0 && cw >= 320 && ch >= 180 && cw <= d.w && ch <= d.h &&
                        (cw * 9 > ch * 16 ? cw * 9 - ch * 16 : ch * 16 - cw * 9) <= cw * 9 / 50;
        return ok ? crop : (d.w << 16 | d.h);
    }
    /* A game that draws a 16:9 picture away from the surface's top-left corner (a GL app on a 1920x1080
     * surface presents (0,360)-(1280,1080)): the VIC samples that window, not the whole surface. */
    bool OffsetWindow(const SrcDesc &d, u32 *x, u32 *y, u32 *w, u32 *h) const
    {
        const u32 ww = cap_win >> 16, wh = cap_win & 0xFFFF, wx = cap_win_xy >> 16, wy = cap_win_xy & 0xFFFF;
        const bool ok = cap_win != 0 && ww >= 320 && wh >= 180 && wx + ww <= d.w && wy + wh <= d.h &&
                        (ww * 9 > wh * 16 ? ww * 9 - wh * 16 : wh * 16 - ww * 9) <= ww * 9 / 50;
        if (ok)
        {
            *x = wx;
            *y = wy;
            *w = ww;
            *h = wh;
        }
        return ok;
    }
    void CropToPresent(SrcDesc &d)
    {
        const u32 use = PresentRect(d, cap_crop);
        if (use != crop_used)
        {
            if (crop_used != 0 || use != (d.w << 16 | d.h))
            {
                LogLine("   nvf: the game presents %ux%u of its %ux%u picture - the VIC %s", use >> 16, use & 0xFFFF,
                        d.w, d.h, use == (d.w << 16 | d.h) ? "converts all of it" : "converts only that part");
            }
            crop_used = use;
        }
        d.rect_w = use >> 16;
        d.rect_h = use & 0xFFFF;
    }

    /* RGB -> BT.709 NV12, 16-row block-linear, into the arena */
    bool Vic(const char *stage, u32 cur = 0)
    {
        g_strip_src = src;
        if (!size_check_ok)
        {
            CropToPresent(g_strip_src);
        }
        else if (u32 wx = 0, wy = 0, ww = 0, wh = 0; !corner && OffsetWindow(g_strip_src, &wx, &wy, &ww, &wh))
        {
            g_strip_src.rect_x = wx;
            g_strip_src.rect_y = wy;
            g_strip_src.rect_w = ww;
            g_strip_src.rect_h = wh;
        }
        g_strip_out = Layout720().out;
        g_strip_flip = present_flip;
        ON_SCOPE_EXIT
        {
            g_strip_flip = 0;
        };
        JobCtx vc{vfd, cmd_handle, vsyncpt, cfg_addr, arena_vic + (cur != 0 ? cur : Layout720().off_cur), slot_vic, 0,
                  0,   0,          0,       SelfSrc};
        vc.out_is_dst_buf = false;
        g_csc_override = std::addressof(NvfBt709);
        const bool ok = RunOneJob(stage, VicJob::BlitStrip, true, vc);
        g_csc_override = nullptr;
        return ok;
    }

    void SetSetupByte(u32 off, u8 v)
    {
        x.a[NvfOffSetup + off] = v;
        armDCacheFlush(x.a + NvfOffSetup + (off & ~0x3Fu), 0x40);
    }

    /* true: the job finished after all - the engine is not wedged */
    bool Stall(const char *who, const char *what)
    {
        LogLine("   %s: %s: no status from our job in 1 s.", who, what);
        u32 now = 0;
        /* late is not wedged. In Minecraft our job waited behind
         * someone else's (grc's recorder, most likely: the syncpoint went
         * 64 past our fence with nothing more of ours submitted) and was
         * done within a second - but this used to end streaming for the
         * whole boot. Give it 3 s more; if the fence is reached the job
         * is over, the channel can close normally, and the next session
         * starts afresh. */
        if (const u32 cfd = CtrlFd(); cfd != 0)
        {
            const u64 w0 = armTicksToNs(armGetSystemTick());
            for (;;)
            {
                struct
                {
                    u32 id;
                    u32 value;
                } r = {x.esyncpt, 0};
                u32 e = 0;
                NvIoctl(cfd, NvHostIocCtrlSyncptRead, std::addressof(r), sizeof(r), std::addressof(e));
                now = r.value;
                if (e == 0 && static_cast<s32>(now - g_nvf_last_fence) >= 0)
                {
                    LogLine(
                        "   %s: the job finished %llu ms later (syncpt %u now %u, fence %u) - NVENC was busy, not wedged; the stream restarts",
                        who, static_cast<unsigned long long>((armTicksToNs(armGetSystemTick()) - w0) / 1000000),
                        x.esyncpt, now, g_nvf_last_fence);
                    return true;
                }
                if (armTicksToNs(armGetSystemTick()) - w0 > UINT64_C(3000000000))
                {
                    break;
                }
                os::SleepThread(TimeSpan::FromMilliSeconds(10));
            }
        }
        g_engine_wedged = true;
        keep_open = true;
        LogLine("   %s: still not done after 3 s more. Leaving the NVENC channel OPEN.", who);
        LogLine("   %s: syncpt %u now %u, fence %u %s; status picture index %#x %s", who, x.esyncpt, now,
                g_nvf_last_fence, g_nvf_last_fence_done ? "REACHED" : "not reached", g_nvf_last_pic,
                g_nvf_last_ours ? "(ours)" : "(not ours - the engine never wrote its status)");
        LogLine("   No further engine work this boot.");
        return false;
    }

    /* the clock, re-ensured: before the first submit, and in a long run
     * whenever a periodic read finds it below 400 MHz */
    bool EnsureClock(const char *who, u32 *hz)
    {
        const u32 ensured = ClockEnsure(PcvModule_NVENC, who);
        *hz = 0;
        return ensured != 0 && ClockRateOf(PcvModule_NVENC, hz) && *hz != 0;
    }
};

/* a read fails for one expected reason - the game was closed */
const char *SlotReadFailure()
{
    return DebugPumpTargetGone() ? "slot read failed: the game exited" : "slot read failed";
}

/* P-frame buffers, past the IDR arena ( nvp;  nvstream with nvgop) */
constexpr u32 NvpOffSetupP = NvfArenaSize;
constexpr u32 NvpOffRefB = NvpOffSetupP + 0x1000;
constexpr u32 NvpOffMeA = NvpOffRefB + 0x160000;
constexpr u32 NvpOffMeB = NvpOffMeA + 0x40000; /* grc's MEPRED buffers are 128 KB apart */
constexpr u32 NvpArenaSize = NvpOffMeB + 0x40000;
constexpr u32 SetupFrameNum = __builtin_offsetof(nvenc_h264_drv_pic_setup_s, pic_control) +
                              __builtin_offsetof(nvenc_h264_pic_control_s, frame_num);
constexpr u32 SetupRcQpP = SetupRc + __builtin_offsetof(nvenc_h264_rc_s, QP); /* QP[P] */
static_assert(SetupFrameNum == 0x17C && nvenc_grc_p::Setup[SetupFrameNum] == 1 &&
                  nvenc_grc_p::Setup[SetupFrameNum + 2] == 2,
              "grc's P setup: frame_num 1 at 0x17C, POC lsb 2 at 0x17E");
static_assert(nvenc_grc_p::Setup[0x1A8] == 0x90 && nvenc_grc_idr::Setup[0x1A8] == 0x9C,
              "pic_control word: P (type 0, ref 1) vs IDR (type 3, ref 1)");
static_assert(sizeof(nvenc_grc_p::Setup) <= 0x1000);
static_assert((SetupFrameNum & ~0x3Fu) == ((SetupFrameNum + 3) & ~0x3Fu), "frame_num and POC share a cache line");

/* the 720p layout is exactly the constants every 720p run used */
const NvfLayout &Layout720()
{
    static constexpr NvfLayout L = {
        NvfW,
        NvfH,
        NvfOut,
        NvfOffSetup,
        NvfOffStatus,
        NvfOffRc,
        NvfOffCmd,
        NvfOffBits,
        NvfOffHist,
        NvfOffCur,
        NvfOffCurUV,
        NvfOffRefOut,
        NvfArenaSize,
        NvpOffSetupP,
        NvpOffRefB,
        NvpOffMeA,
        NvpOffMeB,
        NvpArenaSize,
        nvenc_grc_idr::Setup,
        nvenc_grc_p::Setup,
        nvenc_grc_hdrs::SpsPps,
        static_cast<u32>(sizeof(nvenc_grc_hdrs::SpsPps)),
    };
    return L;
}

/* the VIC writes 1080 rows into planes that are 1088 rows (16-row
 * blocks); NVENC codes all 1088 (nvenc_1080p.h: 1920x1088 surfaces,
 * the only 1080p-class size that completes) and the SPS in
 * SpsPps crops the bottom 8. */

/* ---- : screenshots ----------------------------------------------- */

/* what the session just read into g_ind_buf: the handheld corner, or
 * the whole surface */

// Submit one frame at a time; recover with an IDR after an encode failure.
StreamEnd TryNvencStream(::ams::svc::Handle dbg, u64 slot_base, u32 vfd, u32 nvmap_fd, u32 cmd_handle, u32 vsyncpt,
                         u32 cfg_addr, const GameSurface *geo, bool composed)
{
    VicStage("ns:1");
    g_source_epoch.fetch_add(1, std::memory_order_relaxed);
    /* true mode streams until the viewer or the game goes away */
    constexpr u32 nframes = ~0u;
    /* true progress every ~20 s, with per-window timings, so a
     * mid-run change (a clock, a scene) shows up in the log */
    const u32 progress_every = 1200;
    const u8 qp = static_cast<u8>(g_nvstream_qp < 10 ? 10 : (g_nvstream_qp > 40 ? 40 : g_nvstream_qp));
    u32 gop = g_nvstream_gop > 250 ? 250 : g_nvstream_gop; /* frame_num and POC lsb wrap at 256 */
    if (gop == 1)
    {
        gop = 0;
    }
    if (gop != 0 && (g_ind_buf == nullptr || g_ind_size < NvfArenaBase + NvpArenaSize))
    {
        LogLine("   nvstream: nvgop=%u needs %zu KB of capture region, have %zu KB - streaming IDR-only", gop,
                (NvfArenaBase + NvpArenaSize) / 1024, g_ind_size / 1024);
        gop = 0;
    }
    LogLine("   ---- LIVE H.264 STREAM: until the viewer or the game goes, %s%u, QP %u ----",
            gop != 0 ? "an IDR every " : "IDR-only", gop, qp);

    if (g_stream_stage[0] == nullptr || g_stream_stage[1] == nullptr)
    {
        LogLine("   nvstream: no stage buffers");
        return StreamEnd::Failed;
    }
    /* a viewer on the network takes this stream (docked, there is
     * no USB at all); the transport functions follow g_tx_net */
    /* the receiver should already be running; give a late one 5 s */
    for (u32 t = 0; t < 50 && !UsbReady(); ++t)
    {
        os::SleepThread(TimeSpan::FromMilliSeconds(100));
    }
    if (!UsbReady())
    {
        LogLine("   nvstream: USB not Configured (no host, or \"usb\" absent from the arm file) - not streaming");
        VicStage("ns:no_usb");
        return StreamEnd::ViewerGone;
    }
    NvfSession s;
    if (composed)
        s.SetComposed();
    if (!composed && geo != nullptr && !s.SetGeometry(*geo, "nvstream"))
    {
        VicStage("ns:geometry");
        return StreamEnd::Failed;
    }
    if (!composed && geo != nullptr)
    {
        s.PrepareSlots(dbg, slot_base, "nvstream");
    }
    /* the first frame decides the size. A 1280x720 picture in the
     * corner of the 1920x1080 surface (handheld) streams at 720p; full
     * 1920x1080 content (docked, or a game told it is docked) streams
     * at native 1080p (measured at 10.5 ms of work per 1080p
     * frame). "cap720" in the arm file keeps the old downscale. */
    if (!composed)
    {
        u64 rn = 0;
        u32 sg = 0;
        if (!s.Capture(dbg, slot_base, std::addressof(rn), std::addressof(sg)))
        {
            LogLine("   nvstream: %s", SlotReadFailure());
            return StreamEnd::GameGone;
        }
        s.Decide("nvstream");
    }
    /* any 1920x1080 surface, not only MK8's layout.
     */
    /* a surface other than 1920x1080 stays at 720p even when
     * more than 1280x720 of it is shown - SMO docked (1600x900, dynamic
     * resolution below that) at 1080p looked no better than 720p at the
     * same bitrate, and cost fps and a frame of latency (Run v0713-smo2) */
    const NvfLayout &L = Layout720();
    /* at 1080p one encode stays in flight while the next frame is
     * read and converted into the other picture (Run T2: read 12.4 +
     * VIC 2.2 + NVENC 5.6 ms, run one after another = 37 fps). It costs
     * about a frame of latency, so 720p (which fits 16.7 ms as it is)
     * keeps the sequential path. */
    s.arena_size =
        (gop != 0 ? L.arena_size_p : L.arena_size); /* same base: past both stream stages */
    LogLine("   nvstream: encoding %ux%u%s%s", L.w, L.h,
            (s.corner ? " (the handheld picture, 1:1)" : " (the VIC scales the picture)"),
            "");
    if (!s.Open(vfd, nvmap_fd, cmd_handle, vsyncpt, cfg_addr, "nvstream"))
    {
        VicStage("ns:open_FAILED");
        return g_engine_wedged ? StreamEnd::EngineStall : StreamEnd::Failed;
    }
    SetViewerReason(ViewerReason_None);

    ON_SCOPE_EXIT
    {
        UsbByeStop();
    };
    // P frames use QP+2 and temporal motion hints; IDR frames retain the current base QP.
    /* the QP this stream codes at now - the quality setting, or
     * higher while the network rate control needs it (below) */

    u8 cur_qp = NxrpBitrate::InitialQp;
    const HwRcConfig hwrc{};
    bool hwrc_on = true;
    u32 hwrc_bps = NxrpBitrate::TargetBps;
    u32 hwrc_errs = 0, hwrc_n = 0, hwrc_qp_sum = 0, hwrc_qp_min = 255, hwrc_qp_max = 0;
    int hwrc_hrd = 0;
    {
        LogLine("   nvstream: hardware rate control: fps %u, CPB %u ms, QP %u-%u, start %u, GOP %u",
                hwrc.fps, hwrc.cpb_ms, hwrc.min_qp, hwrc.max_qp, hwrc.start_qp, hwrc.gop);
    }
    auto prep_p = [&]() {
        u8 *ps = s.x.a + L.off_setup_p;
        std::memcpy(ps, L.p_setup, 0x1000);
        const u8 pq = static_cast<u8>(cur_qp + 2 > 51 ? 51 : cur_qp + 2);
        ps[SetupRcQpP] = pq;
        ps[SetupRcQpI] = pq;
        if (hwrc_on)
        {
            ApplyHwRc(ps, hwrc, hwrc_bps);
        }

        const auto *pic = reinterpret_cast<const nvenc_h264_drv_pic_setup_s *>(ps);
        const u32 mo = pic->pic_control.me_control_offset;
        if (mo != 0 && mo + sizeof(nvenc_h264_me_control_s) <= 0x1000)
        {
            auto *me = reinterpret_cast<nvenc_h264_me_control_s *>(ps + mo);
            me->predsrc.self_temporal_enable = 1;
            me->predsrc.self_temporal_search = 1;
            me->predsrc.self_temporal_refine = 1;
            me->predsrc.self_temporal_stamp_l0 = 1; /* the 9-point shape around it */
        }

        armDCacheFlush(ps, 0x1000);
    };
    if (gop != 0)
    {
        prep_p();
    }
    u32 enc_hz = 0;
    if (!s.EnsureClock("nvstream", std::addressof(enc_hz)))
    {
        LogLine("   nvstream: NOT submitting - NVENC clock not established (%u Hz)", enc_hz);
        VicStage("ns:no_clock");
        return StreamEnd::Failed;
    }
    s.SetSetupByte(SetupRcQpI, cur_qp);
    auto hwrc_apply = [&]() {
        if (!hwrc_on)
        {
            return;
        }
        ApplyHwRc(s.x.a + L.off_setup, hwrc, hwrc_bps);
        armDCacheFlush(s.x.a + L.off_setup, 0x1000);
        if (gop != 0)
        {
            prep_p();
        }
    };
    if (hwrc_on)
    {
        hwrc_bps = (CaptureVideoUsesDirectDelivery()) && CaptureVideoBudgetBps() != 0
                       ? CaptureVideoBudgetBps()
                       : NxrpBitrate::TargetBps;
        hwrc_apply();
    }

    const size_t HdrsLen = L.hdrs_len;
    const u32 max_frame = static_cast<u32>(StreamStageSize - HdrsLen); /* what a stage buffer holds */
    std::memcpy(g_stream_stage[0], L.hdrs, HdrsLen);
    std::memcpy(g_stream_stage[1], L.hdrs, HdrsLen);

    const u32 refs[2] = {L.off_ref_out, L.off_ref_b};
    const u32 mes[2] = {L.off_me_a, L.off_me_b};
    u32 size_changes = 0;
    g_vic_quiet = true;
    u32 parity = 0, urb = 0, sent = 0, errs = 0, dropped = 0, done = 0, clock_fixes = 0;
    /* game audio rides along while this stream runs */

    u32 idrs = 0, pos = 0, n_i = 0, n_p = 0;
    bool pending = false, stalled = false, need_idr = true;
    bool said_long_gop = false;
    u32 key_requests = 0;
    g_viewer_key_request.store(false, std::memory_order_relaxed);
 /* it starts with one anyway */
    SizeCheck size_check(s.corner); /* the 720p/1080p check */
    /* the network rate control's state, and what it did per window */
    const char *why = "all frames sent";
    StreamEnd end = StreamEnd::Done;
    u64 s_read = 0, s_vic = 0, s_enc = 0, s_copy = 0, s_usb = 0, s_bytes = 0, m_work = 0, m_bytes = 0;
    u64 s_bytes_i = 0, s_bytes_p = 0, s_age = 0, m_age = 0;
    /* A single 320 ms frame gave no way to say where the time went. Keep
     * each stage's maximum, and the breakdown of the first stalls
     * (work > 50 ms), logged with the progress lines, not in the loop. */
    u64 m_read = 0, m_vic = 0, m_enc = 0, m_copy = 0, m_usbw = 0;
    struct Stall
    {
        u32 frame;
        u32 read_us, vic_us, enc_us, usbw_us, copy_us;
        u64 at_ms;
    };
    Stall stalls[8] = {};
    u32 n_stalls = 0, n_stalls_logged = 0;
    u32 viewer_gaps = 0; /* M86b: USB waits > 500 ms, each followed by an IDR */
    u32 pic = NvfPictureIndex + 0x100;
    const u32 q0 = g_queue_count.load(std::memory_order_relaxed);
    const u64 loop_t0 = armTicksToNs(armGetSystemTick());
    NxrpBitrate tcp_rate(loop_t0);
    const u32 budget = CaptureVideoUsesDirectDelivery() ? CaptureVideoBudgetBps() : NxrpBitrate::TargetBps;
    if (budget)
        tcp_rate.SetTargetBps(budget);
    LogLine(
        "video target %u bps; aggregate plan 10000000 bps, 10 percent headroom, PCM reserved; shared payload pacer active",
        tcp_rate.VideoTargetBps());
    LogLine("NX-Remote-Play bitrate target %u bps, initial QP %u",
            tcp_rate.VideoTargetBps(), cur_qp);

    if (composed)
        tcp_rate.SetDemandFps(30);
    u64 tcp_pace_ns = 0, tcp_pace_max_ns = 0;
    u32 tcp_pace_waits = 0;
    u64 tcp_budget_sample = loop_t0;
    u32 q_window = q0;
    u64 t_window = loop_t0;
    /* One encode "in flight": submitted, not yet collected. Collecting
     * it waits for NVENC, then sends the frame exactly as before. */
    struct InFlight
    {
        bool active;
        u32 i;
        u32 pic;
        u32 fence;
        u64 t_submit;
        bool is_idr;
        u32 pos;
        u64 present_ns, rn, vic_ns, t0, t1, flush_ns;
    };
    u64 w_read = 0, w_flush = 0, w_vic = 0, w_enc = 0, w_fence0 = 0;
    u32 w_n = 0, w_sent0 = 0, w_fwait0 = 0, w_ready0 = 0, w_waited0 = 0, w_drop0 = 0;
    u64 w_t0 = armTicksToNs(armGetSystemTick());
    InFlight fl = {};
    auto log_stalls = [&]() {
        const u32 upto = n_stalls < 8 ? n_stalls : 8;
        for (; n_stalls_logged < upto; ++n_stalls_logged)
        {
            const Stall &k = stalls[n_stalls_logged];
            LogLine(
                "   nvstream: STALL at frame %u (+%llu ms): read %u us, VIC %u us, NVENC %u us, waiting for the previous USB transfer %u us, copy %u us",
                k.frame, static_cast<unsigned long long>(k.at_ms), k.read_us, k.vic_us, k.enc_us, k.usbw_us, k.copy_us);
        }
    };
    /* the console-side age of what was sent, per A/B half */
    /* where a frame's console age goes, per log window -
     * present -> copied (queue + GPU fence + the copy), -> submitted
     * (VIC, setup), -> noticed encoded, -> sent */
    u64 sp_copied = 0, sp_submit = 0, sp_enc = 0, sp_sent = 0;
    u32 sp_n = 0;
    /* true: stop the loop (why/end set) */
    auto collect = [&](InFlight &f) -> bool {
        f.active = false;
        const u64 w0 = armTicksToNs(armGetSystemTick());
        nvenc_pic_stat_s st = {};
        u64 en = 0;
        const int r = NvfWaitDone(s.x, f.pic, f.fence, f.t_submit, std::addressof(st), std::addressof(en));
        if (r == 2)
        {
            g_vic_quiet = false;
            if (s.Stall("nvstream", f.is_idr ? "stream (IDR)" : "stream (P)"))
            {
                why = "NVENC was late - restarting";
                end = StreamEnd::Failed;
                return true;
            }
            stalled = true;
            why = "NVENC stalled";
            end = StreamEnd::EngineStall;
            return true;
        }
        const u64 t3 = armTicksToNs(armGetSystemTick());
        const u64 enc_wait = t3 - w0; /* what the encode cost us beyond the overlap */
        const u32 bytes = st.total_bit_count / 8;
        if (st.ucode_error_status != 0 || bytes == 0 || st.bitstream_start_pos != 0 || bytes > max_frame)
        {
            ++errs;
            need_idr = true; /* never predict from a frame we did not send */
            pos = 0;
            if (hwrc_on && ++hwrc_errs >= 3)
            {
                hwrc_on = false;
                std::memcpy(s.x.a + L.off_setup + SetupRc, L.idr_setup + SetupRc, sizeof(nvenc_h264_rc_s));
                armDCacheFlush(s.x.a + L.off_setup, 0x1000);
                s.SetSetupByte(SetupRcQpI, cur_qp);
                if (gop != 0)
                {
                    prep_p();
                }
                LogLine("   nvstream: hardware rate control failed %u frames (ucode status %u, %u bytes) - back to constant QP",
                        hwrc_errs, static_cast<u32>(st.ucode_error_status), bytes);
            }
            return false;
        }
        if (hwrc_on)
        {
            hwrc_qp_sum += st.avgQP;
            hwrc_hrd = st.hrdFullness;
            if (st.actual_min_qp_used < hwrc_qp_min)
            {
                hwrc_qp_min = st.actual_min_qp_used;
            }
            if (st.actual_max_qp_used > hwrc_qp_max)
            {
                hwrc_qp_max = st.actual_max_qp_used;
            }
            if (++hwrc_n >= 60)
            {
                LogLine("   nvstream: hwrc avgQP %u (min %u max %u) HRD fullness %d, target %u bps", hwrc_qp_sum / hwrc_n,
                        hwrc_qp_min, hwrc_qp_max, hwrc_hrd, hwrc_bps);
                hwrc_n = hwrc_qp_sum = hwrc_qp_max = 0;
                hwrc_qp_min = 255;
            }
        }
        if (f.is_idr)
        {
            ++idrs;
            need_idr = false;
        }
        tcp_rate.Add(bytes + static_cast<u32>(HdrsLen), f.is_idr);

        pos = f.pos + 1;

        /* the previous transfer has had this whole frame to finish in */
        if (pending)
        {
            size_t got = 0;
            if (!UsbWaitAsync(urb, std::addressof(got)))
            {
                pending = false;
                why = "USB transfer did not complete (viewer gone?)";
                end = StreamEnd::ViewerGone;
                return true;
            }
            ++sent;
            pending = false;
        }
        /* the game audio since the last frame, as its own packet -
         * nothing else is in flight on the endpoint at this point.
         * flags bit 3: 16-bit stereo PCM; width = rate, height = channels. */
        const u64 t4 = armTicksToNs(armGetSystemTick());
        armDCacheFlush(s.x.a + L.off_bits, bytes);
        std::memcpy(g_stream_stage[parity] + HdrsLen, s.x.a + L.off_bits, bytes);
        const u32 payload = static_cast<u32>(HdrsLen + bytes);
        const u64 t5 = armTicksToNs(armGetSystemTick());
        const u64 age = (f.present_ns != 0 && t5 > f.present_ns) ? t5 - f.present_ns : 0;

        if (age != 0 && f.t1 > f.present_ns && f.t_submit >= f.t1 && w0 >= f.t_submit && t5 >= w0)
        {
            sp_copied += f.t1 - f.present_ns;
            sp_submit += f.t_submit - f.t1;
            sp_enc += w0 - f.t_submit;
            sp_sent += t5 - w0;
            ++sp_n;
        }
        SftHdrWire h = {};
        h.magic = 0x52544653u; /* "SFTR" */
        h.version = 2;
        h.flags = static_cast<u16>(2 | (f.is_idr ? 4 : 0)); /* bit 1: H.264 AU; bit 2: IDR */
        h.width = L.w;
        h.height = L.h;
        h.stride = static_cast<u32>(age / 1000); /* console-side age, us */
        h.length = payload;
        h.block_h_log2 = 0;
        h.kind = f.i; /* frame number */
        std::memcpy(g_usb_hdr, std::addressof(h), sizeof(h));
        size_t hs = 0;
        if (!UsbSendBuffer(g_usb_hdr, sizeof(SftHdrWire), std::addressof(hs)))
        {
            why = "USB header send failed (viewer gone?)";
            end = StreamEnd::ViewerGone;
            return true;
        }
        if (!UsbPostAsync(g_stream_stage[parity], payload, std::addressof(urb)))
        {
            why = "USB post failed";
            end = StreamEnd::ViewerGone;
            return true;
        }
        pending = true;
        parity ^= 1;
        const u64 t6 = armTicksToNs(armGetSystemTick());
        /* M86b: a USB wait this long means the reader changed (for example a
         * viewer reopened inside the 5 s timeout and joined mid-GOP).
         * Start the next frame from an IDR so it decodes at once. */
        if ((t4 - t3) > UINT64_C(500000000) || (t6 - t5) > UINT64_C(500000000))
        {
            need_idr = true;
            ++viewer_gaps;
        }

        ++done;
        ++w_n;
        w_read += f.rn;
        w_flush += f.flush_ns;
        w_vic += f.vic_ns;
        w_enc += enc_wait;
        s_read += f.rn;
        s_vic += f.vic_ns;
        s_enc += enc_wait;
        s_copy += t5 - t4;
        s_usb += (t4 - t3) + (t6 - t5);
        s_bytes += bytes;
        if (f.is_idr)
        {
            s_bytes_i += bytes;
            ++n_i;
        }
        else
        {
            s_bytes_p += bytes;
            ++n_p;
        }
        s_age += age;
        if (age > m_age)
        {
            m_age = age;
        }
        if (bytes > m_bytes)
        {
            m_bytes = bytes;
        }
        const u64 work = f.rn + f.vic_ns + enc_wait + (t6 - t3);
        if (work > m_work)
        {
            m_work = work;
        }
        if (f.rn > m_read)
        {
            m_read = f.rn;
        }
        if (f.vic_ns > m_vic)
        {
            m_vic = f.vic_ns;
        }
        if (enc_wait > m_enc)
        {
            m_enc = enc_wait;
        }
        if (t4 - t3 > m_usbw)
        {
            m_usbw = t4 - t3;
        }
        if (t5 - t4 > m_copy)
        {
            m_copy = t5 - t4;
        }
        if (work > UINT64_C(50000000))
        {
            if (n_stalls < 8)
            {
                stalls[n_stalls] = {f.i,
                                    static_cast<u32>(f.rn / 1000),
                                    static_cast<u32>(f.vic_ns / 1000),
                                    static_cast<u32>(enc_wait / 1000),
                                    static_cast<u32>((t4 - t3) / 1000),
                                    static_cast<u32>((t5 - t4) / 1000),
                                    (f.t0 - loop_t0) / 1000000};
            }
            ++n_stalls;
        }
        return false;
    };

    /* while Capture waits for the GPU to finish the next frame,
     * collect and send the previous one as soon as its encode is done
     * (fences averaged 4.7 ms of otherwise idle waiting) */

    /* the overlay's fps readout, refreshed about once a second */
    u64 o_t0 = loop_t0;
    u32 o_q0 = q0;
    for (u32 i = 0; i < nframes; ++i)
    {
        u64 rn = 0;
        u32 sg = 0;
        u64 t0 = armTicksToNs(armGetSystemTick());
        {
            if (NxrpSourceDone())
            {
                why = "hybrid capture limit or source fault";
                end = StreamEnd::Done;
                break;
            }
            if (composed == HybridGameFocused())
            {
                why = "hybrid source focus changed";
                end = StreamEnd::Reconfigure;
                break;
            }
        }
        if (g_applet_needs_memory.load(std::memory_order_relaxed))
        {
            why = "an applet (manual, news, eShop) needs the memory";
            end = StreamEnd::Disabled;
            break;
        }

        if ((i & 7) == 0 && UsbViewerSaidBye())
        {
            why = "the viewer closed";
            end = StreamEnd::ViewerGone;
            break;
        }
        /* exp/detach-on-home: game stopped presenting (HOME / SwitchU).
         * End the session so we are not holding a debug attach through
         * the Album/vi ; RunLive reattaches on the next present. */
        /* streaming the HOME menu and a game comes to the front */
        // collect the previous encode before waiting for transport
        // credit. Wait BEFORE reading a raw frame or encoding its P-frame:
        // skipped presents are safe; throwing away encoded references is not.
        if (!CaptureVideoUsesDirectDelivery())
        {
            if (fl.active && collect(fl))
            {
                break;
            }
            const u64 pace_begin = armTicksToNs(armGetSystemTick());
            bool pace_stop = false;
            while (!CaptureVideoCanProduce())
            {
                if (!CaptureVideoViewerPresent())
                {
                    why = "viewer left during capture pacing";
                    end = StreamEnd::ViewerGone;
                    pace_stop = true;
                    break;
                }
                if (composed == HybridGameFocused())
                {
                    why = "hybrid source focus changed during capture pacing";
                    end = StreamEnd::Reconfigure;
                    pace_stop = true;
                    break;
                }
                if (g_applet_needs_memory.load(std::memory_order_relaxed))
                {
                    why = "stream disabled/applet needs memory during pacing";
                    end = StreamEnd::Disabled;
                    pace_stop = true;
                    break;
                }
                if (NxrpSourceDone())
                {
                    why = "source limit/fault during pacing";
                    end = StreamEnd::Done;
                    pace_stop = true;
                    break;
                }

                os::SleepThread(TimeSpan::FromMilliSeconds(1));
            }
            const u64 pace_ns = armTicksToNs(armGetSystemTick()) - pace_begin;
            if (pace_ns >= 1000000ULL)
            {
                ++tcp_pace_waits;
                tcp_pace_ns += pace_ns;
                if (pace_ns > tcp_pace_max_ns)
                    tcp_pace_max_ns = pace_ns;
            }
            if (pace_stop)
            {
                break;
            }
            t0 = armTicksToNs(armGetSystemTick());
        }
        /* the game made new frame buffers (Minecraft loading a
         * world): the ones we read are gone - start over on the new set */
        if (geo != nullptr && g_session_surface->generation != geo->generation)
        {
            why = "the game made new frame buffers";
            end = StreamEnd::Reconfigure;
            break;
        }
        if (t0 - o_t0 >= UINT64_C(1000000000))
        {
            const u32 qn = g_queue_count.load(std::memory_order_relaxed);
            if (!composed && qn > o_q0)
            {
                tcp_rate.SetDemandFps(
                    static_cast<u32>(static_cast<u64>(qn - o_q0) * UINT64_C(1000000000) / (t0 - o_t0)));
            }
            o_t0 = t0;
            o_q0 = qn;
        }

        const u32 dropped_before = dropped;
        const bool got = s.Capture(dbg, slot_base, std::addressof(rn), std::addressof(sg), std::addressof(dropped));
         /* NVENC done during the copy: send before the VIC */
        if ((got && !composed))
            NxrpGameFrameCaptured(dropped - dropped_before);
        if (!got)
        {
            if ((!composed) && !HybridGameFocused())
            {
                why = "game focus/presents ended; switch to composed";
                end = StreamEnd::Reconfigure;
            }
            else
            {
                why = composed ? "Default-display capture failed/refused" : SlotReadFailure();
                end = StreamEnd::GameGone;
            }
            break;
        }
        const u64 present_ns = armTicksToNs(s.present_tick);
        const u64 t1 = armTicksToNs(armGetSystemTick());

        if (!s.Vic("ns:vic", L.off_cur))
        {
            why = "VIC did not complete";
            end = StreamEnd::Failed;
            break;
        }
        /* a screenshot from the frame just read (the stream
         * pauses while the PNG is written, ~0.5 s) */
        const u64 t2 = armTicksToNs(armGetSystemTick());

        /* collect frame i-1 before touching the setups or the bitstream */
        if (fl.active && collect(fl))
        {
            break;
        }

        /* this frame's place in the GOP: 0 is an IDR */
        /* Network rate control. Docked 1080p on Wi-Fi:
         * the link carried 20-29 Mbps; a fast race at Medium wants
         * ~40, so frames queued - 7-30 fps and 100-350 ms of delay.
         * Every 12 frames: the sender busy most of the time means the
         * link is full - code coarser (QP up, faster the fuller);
         * mostly idle, step back towards the chosen quality. Both
         * setups change only here, between encodes. */
        u8 nq = cur_qp;
        u32 bps = 0;
        const u64 now_ns = armTicksToNs(armGetSystemTick());
        if (CaptureVideoUsesDirectDelivery() && now_ns - tcp_budget_sample >= 200000000ULL)
        {
            tcp_budget_sample = now_ns;
            const u32 budget = CaptureVideoBudgetBps();
            if (budget && budget != tcp_rate.VideoTargetBps())
            {
                tcp_rate.SetTargetBps(budget);
                LogLine("video target changed to %u bps; common encode budget", budget);
                if (hwrc_on)
                {
                    hwrc_bps = tcp_rate.VideoTargetBps();
                    hwrc_apply();
                }
            }
        }
        if (!hwrc_on && tcp_rate.StepBurst(now_ns, cur_qp, std::addressof(nq)) && nq != cur_qp)
        {
            cur_qp = nq;
            s.SetSetupByte(SetupRcQpI, cur_qp);
            if (gop != 0)
            {
                prep_p();
            }
        }
        if (tcp_rate.Step(now_ns, cur_qp, std::addressof(nq), std::addressof(bps)))
        {
            LogLine(
                "NX-Remote-Play bitrate: %u bps, target %u; QP %u -> %u; burst cuts %u peak %u bps",
                bps, tcp_rate.VideoTargetBps(), cur_qp,
                nq, tcp_rate.LastBurstCuts(), tcp_rate.LastBurstPeakBps());
            if (!hwrc_on && nq != cur_qp)
            {
                cur_qp = nq;
                s.SetSetupByte(SetupRcQpI, cur_qp);
                if (gop != 0)
                {
                    prep_p();
                }
            }
        }
        // one target owns congestion/recovery. Intentional
        // token pacing must not trigger the old wall-busy QP loop.

        /* the viewer asked for one (a new decoder, a recording
         * starting). Over TCP nothing is lost, so a keyframe is only
         * for such a start: to a viewer that asks, the network sends
         * one every 240 frames - each is up to ~20 P frames' worth,
         * and on a busy link it held the stream up ~0.2 s every second
         * (creeper3010, #7) - the P frames get those bits instead */
        if (g_viewer_key_request.exchange(false, std::memory_order_relaxed))
        {
            need_idr = true;
            ++key_requests;
        }
        const u32 gop_now = ((((CaptureVideoUsesDirectDelivery())))) &&
                                    gop != 0
                                ? 240
                                : gop;
        if (gop_now != gop && !said_long_gop)
        {
            said_long_gop = true;
            LogLine("   nvstream: network GOP240; IDR on connect/source restart/host recovery (setting: %u)", gop);
        }
        if (gop == 0 || need_idr || pos >= gop_now)
        {
            pos = 0;
        }
        NvfJob j;
        j.setup = L.off_setup;
        j.ref_out = refs[pos & 1];
        j.cur = L.off_cur;
        j.cur_uv = L.off_cur_uv;
        j.control = hwrc_on ? NvfControlHwRc : NvfControlConstQp;
        if (pos == 0)
        {
            s.SetSetupByte(SetupIdrPicId, static_cast<u8>(idrs & 1));
        }
        else
        {
            const u16 fn = static_cast<u16>(pos % nvenc_grc_p::MaxFrameNum);
            const u16 poc = static_cast<u16>((2 * pos) % nvenc_grc_p::MaxPocLsb);
            std::memcpy(s.x.a + L.off_setup_p + SetupFrameNum, std::addressof(fn), 2);
            std::memcpy(s.x.a + L.off_setup_p + SetupFrameNum + 2, std::addressof(poc), 2);
            armDCacheFlush(s.x.a + L.off_setup_p + (SetupFrameNum & ~0x3Fu), 0x40);
            j.setup = L.off_setup_p;
            j.ref_in = refs[(pos - 1) & 1];
            j.me_in = mes[(pos - 1) & 1];
            j.me_out = mes[pos & 1];
        }
        fl = InFlight{true, i, pic++, 0, 0, pos == 0, pos, present_ns, rn, t2 - t1, t0, t1, s.last_flush_ns};
        if (NvfSubmit(s.x, fl.pic, j, std::addressof(fl.fence), std::addressof(fl.t_submit)) != 0)
        {
            fl.active = false;
            why = "NVENC submit rejected";
            end = StreamEnd::Failed;
            break;
        }
        if (collect(fl))
        {
            break;
        }

        /* -: has the game switched between a 720p and a
         * 1080p picture (docked/undocked, ReverseNX)? SizeCheck: a few
         * 4-byte reads around the 720p corner and the game's crop, every
         * 250 ms. */
        if (s.size_check_ok)
        {
            const s32 sl = g_queue_slot.load(std::memory_order_relaxed);
            if (const char *r = size_check.Step(dbg, s.SlotAddr(slot_base, sl), sl, s.src.stride_px, s.src.blk_h_log2))
            {
                ++size_changes;
                why = r;
                end = StreamEnd::Reconfigure;
                break;
            }
        }
        /* every ~10 s (~60 s true): the clock (re-ensured if it fell)
         * and a progress line, so a stream that dies midway says how
         * far it got */
        if ((i + 1) % progress_every == 0)
        {
            log_stalls();
            {
                const u64 wn = armTicksToNs(armGetSystemTick());
                const u32 wd = w_n ? w_n : 1;
                const u32 fw = s.fence_waits - w_fwait0;
                const u64 wfps = wn > w_t0 ? static_cast<u64>(sent - w_sent0) * UINT64_C(10000000000) / (wn - w_t0) : 0;
                {
                    u32 cpu_hz = 0, emc_hz = 0;
                    static_cast<void>(ClockRateOf(0x40000001u /* CPU */, std::addressof(cpu_hz)));
                    static_cast<void>(ClockRateOf(0x40000039u /* EMC */, std::addressof(emc_hz)));
                    const u32 sn = sp_n ? sp_n : 1;
                    LogLine(
                        "   nvstream age split avg us: present->copied %llu, ->submitted %llu, ->encoded %llu, ->sent %llu (%u frames); CPU %u MHz, memory %u MHz",
                        static_cast<unsigned long long>(sp_copied / sn / 1000),
                        static_cast<unsigned long long>(sp_submit / sn / 1000),
                        static_cast<unsigned long long>(sp_enc / sn / 1000),
                        static_cast<unsigned long long>(sp_sent / sn / 1000), sp_n, cpu_hz / 1000000, emc_hz / 1000000);
                    sp_copied = sp_submit = sp_enc = sp_sent = 0;
                    sp_n = 0;
                }
                LogLine(
                    "   nvstream window: %llu.%llu fps sent; per frame avg us: read %llu (flush %llu of it)  VIC %llu  NVENC wait %llu  GPU fence %llu; "
                    "presents: %u already drawn, %u waited for, %u skipped",
                    static_cast<unsigned long long>(wfps / 10), static_cast<unsigned long long>(wfps % 10),
                    static_cast<unsigned long long>(w_read / wd / 1000),
                    static_cast<unsigned long long>(w_flush / wd / 1000),
                    static_cast<unsigned long long>(w_vic / wd / 1000),
                    static_cast<unsigned long long>(w_enc / wd / 1000),
                    static_cast<unsigned long long>(fw ? (s.fence_wait_sum - w_fence0) / fw / 1000 : 0),
                    s.pick_ready - w_ready0, s.pick_waited - w_waited0, dropped - w_drop0);
                w_n = 0;
                w_read = w_flush = w_vic = w_enc = 0;
                w_t0 = wn;
                w_sent0 = sent;
                w_fwait0 = s.fence_waits;
                w_fence0 = s.fence_wait_sum;
                w_ready0 = s.pick_ready;
                w_waited0 = s.pick_waited;
                w_drop0 = dropped;
            }
            u32 hz = 0;
            if (!ClockRateOf(PcvModule_NVENC, std::addressof(hz)) || hz < 400000000u)
            {
                ++clock_fixes;
                static_cast<void>(s.EnsureClock("nvstream", std::addressof(hz)));
            }
            const u64 now = armTicksToNs(armGetSystemTick());
            const u32 qn = g_queue_count.load(std::memory_order_relaxed);
            /* the game's own present rate over this window: a frozen game once streamed at
             * 36 fps and only the end-of-run line said so */
            const u64 gw_x10 =
                now > t_window ? static_cast<u64>(qn - q_window) * UINT64_C(10000000000) / (now - t_window) : 0;
            q_window = qn;
            t_window = now;
            LogLine(
                "   nvstream: %u frames in %llu ms, %u sent, avg %llu B/frame (IDR %llu, P %llu), NVENC %u Hz; game presented %llu.%llu fps this window",
                i + 1, static_cast<unsigned long long>((now - loop_t0) / 1000000), sent,
                static_cast<unsigned long long>(done ? s_bytes / done : 0),
                static_cast<unsigned long long>(n_i ? s_bytes_i / n_i : 0),
                static_cast<unsigned long long>(n_p ? s_bytes_p / n_p : 0), hz,
                static_cast<unsigned long long>(gw_x10 / 10), static_cast<unsigned long long>(gw_x10 % 10));
            DebugPumpLogStats("nvstream");
        }
        /* hand the core back: every thread in this process shares core 3
         * with the IPC thread that answers the game (M60b) */
        os::SleepThread(TimeSpan::FromMicroSeconds(500));
    }
    /* never leave the channel with a job in flight : collect it,
     * and send it too if the stream is still healthy */
    if (fl.active)
    {
        if (end == StreamEnd::Done || end == StreamEnd::Reconfigure || end == StreamEnd::GameGone ||
            end == StreamEnd::Disabled)
        {
            static_cast<void>(collect(fl));
        }
        else
        {
            nvenc_pic_stat_s st = {};
            u64 en = 0;
            fl.active = false;
            if (NvfWaitDone(s.x, fl.pic, fl.fence, fl.t_submit, std::addressof(st), std::addressof(en)) == 2)
            {
                if (!s.Stall("nvstream", "the last frame in flight"))
                {
                    stalled = true;
                }
            }
        }
    }
    if (pending && end != StreamEnd::ViewerGone)
    { /* a gone viewer's transfer was cancelled */
        size_t got = 0;
        if (UsbWaitAsync(urb, std::addressof(got)))
        {
            ++sent;
        }
    }
    g_vic_quiet = false;
    const u64 wall = armTicksToNs(armGetSystemTick()) - loop_t0;
    const u32 presented = g_queue_count.load(std::memory_order_relaxed) - q0;
    const u32 nd = done ? done : 1;
    const u64 fps_x10 = wall ? static_cast<u64>(sent) * UINT64_C(10000000000) / wall : 0;
    const u64 game_x10 = wall ? static_cast<u64>(presented) * UINT64_C(10000000000) / wall : 0;

    LogLine("   nvstream: stopped: %s", why);
    SupportPools("stream stopped");

    LogLine(
        "   nvstream: %u frames sent in %llu ms -> %llu.%llu fps (game presented %llu.%llu fps, %u presents skipped), %u encodes with errors",
        sent, static_cast<unsigned long long>(wall / 1000000), static_cast<unsigned long long>(fps_x10 / 10),
        static_cast<unsigned long long>(fps_x10 % 10), static_cast<unsigned long long>(game_x10 / 10),
        static_cast<unsigned long long>(game_x10 % 10), dropped, errs);
    LogLine(
        "   nvstream: per frame avg us: read+flush %llu  VIC %llu  NVENC %llu  copy %llu  usb %llu; worst work %llu us",
        static_cast<unsigned long long>(s_read / nd / 1000), static_cast<unsigned long long>(s_vic / nd / 1000),
        static_cast<unsigned long long>(s_enc / nd / 1000), static_cast<unsigned long long>(s_copy / nd / 1000),
        static_cast<unsigned long long>(s_usb / nd / 1000), static_cast<unsigned long long>(m_work / 1000));
    LogLine(
        "   nvstream: avg %llu B/frame (max %llu) -> %llu Mbps at the achieved rate; %u IDR avg %llu B, %u P avg %llu B; clock re-ensured %u time(s)",
        static_cast<unsigned long long>(s_bytes / nd), static_cast<unsigned long long>(m_bytes),
        static_cast<unsigned long long>(wall ? s_bytes * 8 * 1000 / wall : 0), n_i,
        static_cast<unsigned long long>(n_i ? s_bytes_i / n_i : 0), n_p,
        static_cast<unsigned long long>(n_p ? s_bytes_p / n_p : 0), clock_fixes);
    LogLine("   nvstream: console latency, source acquisition begin -> adapter sent: avg %llu us, max %llu us",
            static_cast<unsigned long long>(s_age / nd / 1000), static_cast<unsigned long long>(m_age / 1000));
    LogLine(
        "   nvstream: present fences: %u waited, avg %llu us, max %llu us, %u timed out (250 ms), %u presents without a readable fence; %u read(s) redone (the slot may have been reused while it was read)",
        s.fence_waits, static_cast<unsigned long long>(s.fence_waits ? s.fence_wait_sum / s.fence_waits / 1000 : 0),
        static_cast<unsigned long long>(s.fence_wait_max / 1000), s.fence_timeouts, s.fence_unknown, s.reread);
    LogLine(
        "   nvstream: presents taken: %u already drawn, %u waited for (the oldest not yet drawn), %u by the old latest-present rule",
        s.pick_ready, s.pick_waited, s.pick_legacy);
    LogLine(
        "   nvstream: per-stage max us: read %llu  VIC %llu  NVENC %llu  USB wait %llu  copy %llu; %u frame(s) over 50 ms of work; %u USB wait(s) over 500 ms (IDR forced after each)",
        static_cast<unsigned long long>(m_read / 1000), static_cast<unsigned long long>(m_vic / 1000),
        static_cast<unsigned long long>(m_enc / 1000), static_cast<unsigned long long>(m_usbw / 1000),
        static_cast<unsigned long long>(m_copy / 1000), n_stalls, viewer_gaps);
    for (u32 k = n_stalls_logged; k < (n_stalls < 8 ? n_stalls : 8); ++k)
    {
        LogLine(
            "   nvstream: STALL at frame %u (+%llu ms): read %u us, VIC %u us, NVENC %u us, waiting for the previous USB transfer %u us, copy %u us",
            stalls[k].frame, static_cast<unsigned long long>(stalls[k].at_ms), stalls[k].read_us, stalls[k].vic_us,
            stalls[k].enc_us, stalls[k].usbw_us, stalls[k].copy_us);
    }
    DebugPumpLogStats("nvstream");
    if (size_changes != 0)
    {
        LogLine("   nvstream: ended to restart at the game's new picture size");
    }
    if (key_requests != 0)
    {
        LogLine("   nvstream: %u keyframe(s) the viewer asked for (a new decoder, a recording)", key_requests);
    }
    LogLine("capture pacing: %u waits, total %llu ms, max %llu ms; encoded reference frames preserved",
            tcp_pace_waits, static_cast<unsigned long long>(tcp_pace_ns / 1000000),
            static_cast<unsigned long long>(tcp_pace_max_ns / 1000000));

    /* the drift test's reference - the VIC's picture of the last
     * frame that went out, and which frame that was. Only when that
     * frame is also the last one the VIC converted. */
    VicStage(stalled ? "ns:STALLED_left_open" : (sent == nframes ? "ns:DONE" : "ns:stopped"));
    return end;
}

/* ---- : the grc observer ---------------------------------------
 *
 * - cost three forced power-offs. Every one came from the same
 * mechanism: we opened a channel to an engine, submitted work that
 * never completed, and the teardown took the compositor with it.
 * host1x is shared and nvnflinger is on the other side of it.
 *
 * So this observes and submits nothing at all. No channel, no cmdbuf,
 * no engine contact of any kind. It attaches to grc - the system's own
 * recorder, the one client known to drive NVENC successfully on this
 * firmware - resumes it immediately, reads its memory, and detaches.
 *
 * That is exactly the mechanism already used on the game's framebuffer
 * at 60 fps without incident, pointed at a different process. The one
 * thing at risk is grc itself, not the display.
 *
 * What we are looking for is grc's nvenc_h264_drv_pic_setup_s. Its
 * first word is a magic constant, which makes it findable by scanning,
 * and it is the single most valuable artifact available: the exact
 * configuration a working client hands this engine. Also searched for
 * is a host1x SETCL to class 0x21, which would give the real method
 * sequence - including any method we never emit. */

void VicWorkerThread(void *)
{
    bool ui_started = false;
    for (;;)
    {
        os::SleepThread(TimeSpan::FromMilliSeconds(200));
        /* with home_menu = 1 the engines do not wait for the
         * first game's present - the HOME menu streams from boot
         * (test2: nothing until a game was opened) */
        if (!ui_started && g_vic_pending.load(std::memory_order_acquire) < 0 && armTicksToNs(armGetSystemTick()) >= static_cast<u64>(g_probe_delay_s) * UINT64_C(1000000000))
        {
            ui_started = true;
            LogLine("live: home_menu - starting without waiting for a game");
            g_last_slot = 0;
            RunVicBlit(0);
            while (g_rearm.exchange(false))
            {
                RunVicBlit(g_last_slot);
            }
        }
        const s32 slot = g_vic_pending.exchange(-1, std::memory_order_acq_rel);
        if (slot >= 0)
        {
            g_last_slot = slot;
            RunVicBlit(slot);
        }
        while (g_rearm.exchange(false))
        {
            RunVicBlit(g_last_slot);
        }
    }
}

} // namespace

void StartVicWorker()
{
    /* M60b: deliberately LOWER priority than main/IPC (higher number is
     * lower priority on Horizon). All our threads share core 3, so a worker
     * at equal priority round-robins against the IPC thread and the game
     * blocks on binder calls we never get scheduled to answer. Below it,
     * IPC preempts us the moment a request arrives and the worker simply
     * uses what is left. */
    const s32 worker_prio = os::GetThreadPriority(os::GetCurrentThread()) + 4;
    NXRP_ABORT_UNLESS(os::CreateThread(std::addressof(g_vic_thread), VicWorkerThread, nullptr, g_vic_stack,
                                       sizeof(g_vic_stack), worker_prio));
    os::SetThreadNamePointer(std::addressof(g_vic_thread), "applet-mitm.VIC");
    os::StartThread(std::addressof(g_vic_thread));
    LogLine("VIC worker started");
}

void RequestVicBlit(s32 slot)
{
    /* binder thread: hand off and get out. */
    g_vic_pending.store(slot, std::memory_order_release);
}

} // namespace ams::mitm::applet
