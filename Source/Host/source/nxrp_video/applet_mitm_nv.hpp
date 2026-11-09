/*
 * applet-mitm - hand-rolled nvdrv access + VIC hardware blit.
 *
 * Stage 1 (verified): open nvdrv:s, import the game's swapchain nvmap, survey
 * engines, allocate our own linear buffer, prove the VIC channel is usable.
 *
 * Stage 2 (this file): actually drive the VIC. Capture the swapchain geometry
 * from the setPreallocatedBuffer parcels, then on a later queueBuffer run one
 * NVHOST_IOCTL_CHANNEL_SUBMIT that de-swizzles + format-converts a crop of the
 * live frame (block-linear) into our linear buffer, wait the syncpoint, and
 * checksum the result.
 */
#pragma once
#include <stratosphere.hpp>
#include <atomic>
#include "applet_mitm_gbuf.hpp"

namespace ams::mitm::applet
{

/* Swapchain description, filled from the first few setPreallocatedBuffer
 * parcels. MK8 registers 3 slots, all inside ONE nvmap object (id 1268),
 * at offsets 0 / 0x870000 / 0x10E0000. */
struct GameSurface
{
    u32 nvmap_id;       /* the nvmap object id to import                */
    u32 width;          /* plane[0] width  in pixels                    */
    u32 height;         /* plane[0] height in pixels                    */
    u32 stride_px;      /* plane[0] pitch bytes / 4                     */
    u32 block_h_log2;   /* plane[0] block_height_log2 (MK8: 4)          */
    u32 pix_format;     /* vic::PIXFMT_* for plane[0]                   */
    u32 slot_offset[8]; /* plane[0].offset per registered slot          */
    u32 num_slots;
    bool armed; /* >=1 slot captured, blit not yet done         */
    /* per-game geometry for live mode */
    u32 buf_size;      /* one buffer's size (NvGraphicBuffer total_size) */
    u64 color_format;  /* plane[0] NvColorFormat, raw                  */
    u32 layout;        /* plane[0] NvLayout: 3 = BlockLinear           */
    u32 generation;    /* bumped when a new buffer set (nvmap id) starts */
    u32 slot_nvmap[8]; /* each slot's nvmap id - Minecraft gives
                        * every slot its own buffer object */
};
extern GameSurface g_game_surface;

/* Record one setPreallocatedBuffer's NvGraphicBuffer into g_game_surface. */
/* `slot` is the swapchain slot the parcel names (not a running
 * count), or -1 if it could not be parsed. */
void CaptureGameSurface(const NvGraphicBufferRaw *gb, s32 slot);

/* the Album / hbmenu (an applet with buffers of its own, like a
 * game's) - its geometry apart, so a suspended game's survives it */
extern GameSurface g_applet_surface;
void CaptureAppletSurface(const NvGraphicBufferRaw *gb, s32 slot);
/* the Album's process (hbmenu or the real one) once it sent a frame, and
 * the process whose present was recorded last */
extern std::atomic<u64> g_applet_target_pid;
extern std::atomic<u64> g_present_pid;
/* the system tick of the application's last present, recorded or not
 * (a UI session only notes them) - "the game is on screen" */
extern std::atomic<u64> g_app_present_tick;
inline bool IsAlbumProgram(u64 tid)
{
    return tid == UINT64_C(0x010000000000100D);
}

/* result of pmdmntInitialize() at startup; 0 means the pid lookup is usable */
extern u32 g_pmdmnt_rc;

/* Current probe step, for the heartbeat to snapshot into .last. A run that
 * wedges therefore names the exact ioctl it wedged on. */

/* Written by the binder thread on every queueBuffer (code 7): how many
 * frames the game has presented, and which swapchain slot the last one went
 * into. Both are relaxed atomic stores - the capture loop polls them from
 * the worker thread, and nothing that can block ever touches the binder
 * thread. That rule has held since M8 froze the console. */

extern u32 g_probe_delay_s;

extern u32 g_nvstream_qp;
extern u32 g_nvstream_gop; /* 0 = IDR-only; N = an IDR every N frames, P frames between */

extern std::atomic<u32> g_queue_count;
extern std::atomic<s32> g_queue_slot;
extern std::atomic<u64> g_queue_tick; /* system tick of the latest queueBuffer */
/* the latest queueBuffer's acquire fence - the GPU is done drawing
 * the slot once every syncpoint (id << 32 | value) has reached its value */
extern std::atomic<u32> g_queue_fence_n;
/* the latest queueBuffer's NATIVE_WINDOW_TRANSFORM_* - some games
 * (Kirby's Return to Dream Land Deluxe, Kirby and the Forgotten Land)
 * render upside down and have the compositor flip the picture */
extern std::atomic<u32> g_queue_transform;
extern std::atomic<u64> g_app_pid;             /* applet_mitm_service.cpp: the running application */
extern std::atomic<u64> g_crop_full_pid;       /* the application whose crop was ever 1920x1080 */
extern std::atomic<u32> g_queue_crop_wh;       /* the last present's crop, w << 16 | h (0: none) */
extern std::atomic<bool> g_queue_crop_origin0; /* that crop starts at (0,0) */
extern std::atomic<u32> g_queue_crop_xy;      /* its left << 16 | top when it does not start at (0,0), else 0 */
/* the viewer asked for a keyframe ("SFTK", USB or network), and
 * this network viewer can ask (it has once): keyframes then every 240
 * frames on the network, and on request */
extern std::atomic<bool> g_viewer_key_request;
extern std::atomic<u32> g_source_epoch; /* +1 whenever a video source (game, HOME) starts */
extern std::atomic<u64> g_queue_fence[4];
/* a seqlock over slot + fence: odd while the binder thread writes */
extern std::atomic<u32> g_queue_seq;
// Publish presentation metadata before advancing g_queue_count.
struct PresentRec
{
    std::atomic<u32> seq;
    std::atomic<u32> count;
    std::atomic<s32> slot;
    std::atomic<u32> fence_n;
    std::atomic<u64> fence[4];
    std::atomic<u64> tick;
    std::atomic<u32> transform; /* queueBuffer's transform (bit 0 flip H, bit 1 flip V) */
    std::atomic<u32> crop;      /* its crop, w << 16 | h - 0: none, or not from (0,0) */
    std::atomic<u32> win;       /* a crop not from (0,0): w << 16 | h, else 0 */
    std::atomic<u32> win_xy;    /* that crop's left << 16 | top */
};
constexpr u32 PresentRingSize = 8;
extern PresentRec g_present_ring[PresentRingSize];

/* Record a game or applet present from the display-service wrappers. */
void RecordPresent(s32 slot, bool have_fence, u32 fence_n, const u64 fence[4], u32 transform, const s32 crop[4]);
/* Spawn the worker. The VIC probe MUST NOT run on the binder dispatch
 * thread: doing so blocks the game's queueBuffer, which wedges vi, which
 * forces a power-off, which loses the very log we need. M8b died exactly
 * that way. The worker owns all nvdrv work; a hang there costs us the
 * probe, not the console. */
void StartVicWorker();

/* Called from the binder thread. Records the slot and returns immediately -
 * never blocks, never touches nvdrv. */
void RequestVicBlit(s32 slot);

} // namespace ams::mitm::applet
