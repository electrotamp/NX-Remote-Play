#include <stratosphere.hpp>
#include <cstdio>
#include <cstring>
#include "nxrp_video/applet_mitm_log.hpp"
#include "nxrp_video/applet_mitm_nv.hpp"
#include "nxrp_layers.h"
#include "nxrp_deswizzle.hpp"
#include "nxrp_present.hpp"

//  NXRP : layers source. Read-only: brief debug attach to vi and overlayDisp, nothing is
// mapped, nothing is written to any process. Output is the existing composed-source buffer (1280x720 pitch-linear
// RGBA, bytes R,G,B,A like the screenshot source), so VIC/NVENC/TCP are unchanged. Layout facts MEASURED in /:
// each slot is 1280x768x4 block-linear (GOB 64Bx8, block = 16 GOBs = 8192 B covering 64 B x 128 rows, 80 blocks per
// block-row, 6 block-rows), tap's deswizzle.py layout.
//
//  the vi (HOME / Settings) base layer is read in step with HOME's own presents instead of guessing the
// finished slot from 32 sampled spots. vi keeps the last PresentSharedFrameBuffer request (slot + fence) in an IPC
// buffer; this module finds it once and polls it. When HOME presents slot B the display still
// holds the other slot A and the HOME menu cannot draw into A until the display lets go of it, so A is copied raw in
// one short pass (6 reads, ~3 ms; tap measured 300/300 clean with <= 3.3 ms reads) and only then de-swizzled. A present
// during the copy, or any of 128 spots changing, discards the copy and retries once, then falls back to the screenshot
// source for that frame. With no present for 50 ms the display holds the newest slot and no draw is pending, so that
// slot is read. : while an overlay (overlayDisp) is up the screenshot source is used instead (its raw slot memory
// shows striped/diagonal garbage). Staging is the unused part of the capture region (offset 4 MiB; the composed source
// only uses the first 3.7 MB), so no new memory is taken.

namespace
{

using namespace ams;
using nxrp_present::Msg;

constexpr u64 ViTid = UINT64_C(0x010000000000002D);
constexpr u64 OvTid = UINT64_C(0x010000000000100C);
constexpr u32 W = 1280, H = 720, SlotRows = 768;
constexpr size_t Pitch = W * 4;                        // 5120
constexpr size_t SlotBytes = size_t(W) * SlotRows * 4; // 3,932,160
constexpr u32 BlocksPerRow = 80, BlockBytes = 8192, GobBytes = 512;
constexpr size_t BlockRowBytes = size_t(BlocksPerRow) * BlockBytes; // 655,360
constexpr u32 Spots = 64;                                           // overlay slot pick
constexpr u32 PostSpots = 128;                                      // vi copy verification
constexpr size_t RawOffset = size_t(4) * 1024 * 1024; // staging inside the capture region (see header comment)
constexpr size_t RawNeed = RawOffset + SlotBytes;     // 8,126,464 < 8,847,360 (the region before the stream stages)
constexpr u32 MaxMsgs = 64, MaxRegs = 48;
constexpr size_t ScanChunk = 16 * 1024;
constexpr u64 IdleNs = UINT64_C(50000000);       // no present this long: the display holds the newest slot
constexpr u64 StaleCallNs = UINT64_C(100000000); // longer since the previous frame: first poll proves nothing

alignas(0x1000) constinit u8 g_chunk[ScanChunk + 0x60] = {}; // vi data scan window

struct Target
{
    u64 tid;
    u64 pid;
    u64 base;
    u64 size;
    u32 slots;
    bool ok;
};
struct Region
{
    u64 addr, size;
};
constinit Target g_vi = {ViTid, 0, 0, 0, 0, false};
constinit Target g_ov = {OvTid, 0, 0, 0, 0, false};
constinit bool g_logged_armed = false;
constinit u64 g_next_resolve = 0;
constinit u32 g_last_ov = 0;

// present follower state (vi)
constinit Region g_regs[MaxRegs] = {};
constinit u32 g_nregs = 0;
constinit Msg g_msgs[MaxMsgs] = {};
constinit u32 g_nmsgs = 0;
constinit s32 g_active = -1;
constinit u32 g_seen_val = 0;
constinit bool g_seen_valid = false;
constinit u64 g_next_scan = 0;
constinit u64 g_last_call_end = 0;

// telemetry
constinit u64 n_frames = 0, n_ov_active = 0, n_unstable = 0, n_tear = 0, n_fail = 0, n_fallback = 0;
constinit u64 n_synced = 0, n_idle = 0, n_retry = 0, n_unknown_age = 0, n_foreign = 0, n_scans = 0, n_switch = 0;
constinit u64 att_sum = 0, att_max = 0, rd_sum = 0, rd_max = 0, tot_sum = 0, tot_max = 0;
constinit u64 raw_sum = 0, raw_max = 0, wait_sum = 0, wait_max = 0, dz_sum = 0, dz_max = 0, n_dz_split = 0;
constinit u64 n_ov_shot = 0;
constinit u64 g_ov_hold_until = 0;
constinit bool g_ov_shot_pending = false;
constexpr u64 OverlayHoldNs = UINT64_C(700000000); // screenshot source this long after the overlay was last seen

// de-swizzle worker: the vi copy is 3.9 MB; block-rows 0..2 go to this thread, 3..5 stay on the capture thread
constexpr u32 DzSplit = 3;
constinit const u8 *g_dz_raw = nullptr;
constinit u8 *g_dz_dst = nullptr;
constinit s32 g_dz_state = 0; // 0 not started, 1 running, -1 could not start
os::EventType g_dz_go, g_dz_done;
alignas(os::ThreadStackAlignment) constinit u8 g_dz_stack[16 * 1024] = {};
constinit os::ThreadType g_dz_thread = {};

u64 NowNs()
{
    return armTicksToNs(armGetSystemTick());
}

u64 ProgramOf(u64 pid, os::NativeHandle *out_h)
{
    os::NativeHandle h = os::InvalidNativeHandle;
    ncm::ProgramLocation loc = {};
    cfg::OverrideStatus st = {};
    if (R_FAILED(::ams::pm::dmnt::AtmosphereGetProcessInfo(std::addressof(h), std::addressof(loc), std::addressof(st),
                                                           os::ProcessId{pid})))
    {
        return 0;
    }
    if (out_h != nullptr)
    {
        *out_h = h;
    }
    else
    {
        os::CloseNativeHandle(h);
    }
    return loc.program_id.value;
}

// Find the two-device slot region of a process (vi: 26,880 KB, overlayDisp: 7,680 KB).
bool Resolve(Target &t)
{
    t.ok = false;
    os::ProcessId pid{};
    if (R_FAILED(::ams::pm::dmnt::GetProcessId(std::addressof(pid), ncm::ProgramId{t.tid})) || pid.value == 0)
    {
        return false;
    }
    os::NativeHandle h = os::InvalidNativeHandle;
    if (ProgramOf(pid.value, std::addressof(h)) != t.tid)
    {
        if (h != os::InvalidNativeHandle)
        {
            os::CloseNativeHandle(h);
        }
        return false;
    }
    u64 addr = 0, best = 0, best_size = 0;
    for (u32 guard = 0; guard < 4096; ++guard)
    {
        ::MemoryInfo mi = {};
        u32 pi = 0;
        if (R_FAILED(::svcQueryProcessMemory(&mi, &pi, h, addr)) || mi.size == 0)
        {
            break;
        }
        if (mi.device_refcount >= 2 && mi.size >= SlotBytes * 2 && mi.size > best_size)
        {
            best = mi.addr;
            best_size = mi.size;
        }
        const u64 next = mi.addr + mi.size;
        if (next <= addr)
        {
            break;
        }
        addr = next;
    }
    os::CloseNativeHandle(h);
    if (best == 0)
    {
        return false;
    }
    if (t.pid != pid.value || t.base != best)
    {
        g_nmsgs = 0;
        g_active = -1;
        g_seen_valid = false;
    } // a restarted vi: old message addresses are void
    t.pid = pid.value;
    t.base = best;
    t.size = best_size;
    t.slots = static_cast<u32>(best_size / SlotBytes);
    if (t.slots > 7)
    {
        t.slots = 7;
    }
    t.ok = true;
    return true;
}

u32 DrainContinue(::ams::svc::Handle dbg)
{
    u32 n = 0;
    ::ams::svc::DebugEventInfo ev;
    while (n < 256 && R_SUCCEEDED(::ams::svc::GetDebugEvent(std::addressof(ev), dbg)))
    {
        ++n;
    }
    if (n != 0)
    {
        static_cast<void>(::ams::svc::ContinueDebugEvent(
            dbg, ::ams::svc::ContinueFlag_ExceptionHandled | ::ams::svc::ContinueFlag_ContinueAll, nullptr, 0));
    }
    return n;
}

bool Attach(const Target &t, ::ams::svc::Handle *dbg)
{
    if (R_FAILED(::ams::svc::DebugActiveProcess(dbg, t.pid)))
    {
        return false;
    }
    DrainContinue(*dbg);
    return true;
}

void Detach(::ams::svc::Handle dbg)
{
    DrainContinue(dbg);
    static_cast<void>(::ams::svc::CloseHandle(dbg));
}

// Offset of the i-th verification / sample spot (64 bytes) inside a slot.
inline u64 SpotOffset(u32 i)
{
    return ((static_cast<u64>(i) * 61223 + 17) % (SlotBytes / 64)) * 64;
}

// 32 spots of 64 bytes spread across a slot; written to out[Spots] (hash of each).
bool Sample(::ams::svc::Handle dbg, const Target &t, u32 slot, u64 *out)
{
    u8 b[64];
    for (u32 i = 0; i < Spots; ++i)
    {
        if (R_FAILED(::ams::svc::ReadDebugProcessMemory(reinterpret_cast<uintptr_t>(b), dbg,
                                                        t.base + static_cast<u64>(slot) * SlotBytes + SpotOffset(i),
                                                        sizeof(b))))
        {
            return false;
        }
        u64 h = 0xcbf29ce484222325ull;
        for (u32 k = 0; k < 64; k += 8)
        {
            u64 v;
            std::memcpy(std::addressof(v), b + k, 8);
            h = (h ^ v) * 0x100000001b3ull;
        }
        out[i] = h;
    }
    return true;
}

bool SameSample(const u64 *a, const u64 *b)
{
    return std::memcmp(a, b, sizeof(u64) * Spots) == 0;
}
// Hash of 64 zero bytes as Sample() computes it: a slot spot that never changed from zero hashes to this.
constexpr u64 ZeroSpot()
{
    u64 h = 0xcbf29ce484222325ull;
    for (u32 k = 0; k < 8; ++k)
    {
        h = (h ^ 0) * 0x100000001b3ull;
    }
    return h;
}
bool AnyNonzero(const u64 *s)
{
    for (u32 i = 0; i < Spots; ++i)
    {
        if (s[i] != ZeroSpot())
        {
            return true;
        }
    }
    return false;
}

// overlayDisp's slot to show. Its presents go through the display binder, not vi, so there is no message to follow:
// both slots are sampled twice 0.9 ms apart (a slot unchanged across the gap is not being drawn), and of the finished
// slots the one that changed most recently (against the previous call's sample) is the newest picture; ties keep the
// previous choice. *active: the chosen slot has anything drawn (a cleared overlay is all zero).
bool PickOverlaySlot(::ams::svc::Handle dbg, u32 *chosen, bool *active)
{
    static u64 a[2][Spots], b[2][Spots], prev[2][Spots];
    static u64 changed_at[2] = {0, 0};
    static bool have_prev = false;
    const u32 n = g_ov.slots < 2 ? g_ov.slots : 2;
    if (n == 0)
    {
        return false;
    }
    for (u32 i = 0; i < n; ++i)
    {
        if (!Sample(dbg, g_ov, i, a[i]))
        {
            return false;
        }
    }
    os::SleepThread(TimeSpan::FromMicroSeconds(900));
    for (u32 i = 0; i < n; ++i)
    {
        if (!Sample(dbg, g_ov, i, b[i]))
        {
            return false;
        }
    }
    const u64 now = NowNs();
    for (u32 i = 0; i < n; ++i)
    {
        if (have_prev && !SameSample(prev[i], b[i]))
        {
            changed_at[i] = now;
        }
        std::memcpy(prev[i], b[i], sizeof(prev[i]));
    }
    have_prev = true;
    s32 pick = -1;
    for (u32 i = 0; i < n; ++i)
    {
        if (!SameSample(a[i], b[i]))
        {
            continue;
        }
        if (pick < 0 || changed_at[i] > changed_at[pick] || (changed_at[i] == changed_at[pick] && i == g_last_ov))
        {
            pick = static_cast<s32>(i);
        }
    }
    if (pick < 0)
    {
        ++n_unstable;
        pick = g_last_ov < n ? static_cast<s32>(g_last_ov) : 0;
    }
    g_last_ov = static_cast<u32>(pick);
    *chosen = static_cast<u32>(pick);
    *active = AnyNonzero(b[pick]);
    return true;
}

// ---- vi present follower (tap uisource technique) ----

// vi's writable data except the shared buffer, at most 16 MB: where the IPC message buffers live.
void FindRegions(::ams::svc::Handle dbg)
{
    g_nregs = 0;
    u64 addr = 0, total = 0;
    for (u32 guard = 0; guard < 4096 && g_nregs < MaxRegs; ++guard)
    {
        ::ams::svc::MemoryInfo mi = {};
        ::ams::svc::PageInfo pi = {};
        if (R_FAILED(::ams::svc::QueryDebugProcessMemory(std::addressof(mi), std::addressof(pi), dbg, addr)) ||
            mi.size == 0)
        {
            break;
        }
        const u32 type = static_cast<u32>(mi.state) & 0xFF;
        const bool data = type == 0x04 || type == 0x05 || type == 0x09;
        const bool rw = (static_cast<u32>(mi.permission) & 2) != 0;
        if (data && rw && mi.base_address != g_vi.base && mi.size <= 16 * 1024 * 1024 &&
            total + mi.size <= 16 * 1024 * 1024)
        {
            g_regs[g_nregs++] = {mi.base_address, mi.size};
            total += mi.size;
        }
        const u64 next = mi.base_address + mi.size;
        if (next <= addr)
        {
            break;
        }
        addr = next;
    }
}

// Every present-shaped message in vi's data (header-checked: "SFCI"+8255 or "SFCO"+0).
void ScanMessages(::ams::svc::Handle dbg)
{
    constexpr size_t Lead = nxrp_present::Lead;
    g_nmsgs = 0;
    g_active = -1;
    for (u32 r = 0; r < g_nregs; ++r)
    {
        for (u64 off = 0; off < g_regs[r].size; off += ScanChunk)
        {
            const u64 lead = off >= Lead ? Lead : off;
            const size_t len = static_cast<size_t>(std::min<u64>(ScanChunk, g_regs[r].size - off) + lead);
            if (R_FAILED(::ams::svc::ReadDebugProcessMemory(reinterpret_cast<uintptr_t>(g_chunk), dbg,
                                                            g_regs[r].addr + off - lead, len)))
            {
                continue;
            }
            for (size_t i = Lead; i + 8 <= len; i += 4)
            {
                if (i < lead)
                {
                    continue;
                }
                Msg m;
                if (!nxrp_present::Parse(g_chunk + i, std::addressof(m)))
                {
                    continue;
                }
                if (g_nmsgs < MaxMsgs)
                {
                    m.va = g_regs[r].addr + off - lead + i;
                    m.changed_ns = 0;
                    g_msgs[g_nmsgs++] = m;
                }
            }
        }
    }
}

bool ReadMsg(::ams::svc::Handle dbg, u32 k, Msg *out)
{
    u8 buf[nxrp_present::Window];
    if (R_FAILED(::ams::svc::ReadDebugProcessMemory(reinterpret_cast<uintptr_t>(buf), dbg,
                                                    g_msgs[k].va - nxrp_present::Lead, sizeof(buf))))
    {
        return false;
    }
    if (!nxrp_present::Parse(buf + nxrp_present::Lead, out))
    {
        return false;
    }
    out->va = g_msgs[k].va;
    return true;
}

// Find HOME's present message: scan, then the one whose fence moves within 60 ms (HOME presents at 60 Hz even when
// idle) and whose slot is in the 3/4 pair. Rate-limited: a scan reads ~7 MB.
bool FindActive(::ams::svc::Handle dbg)
{
    const u64 now = NowNs();
    if (now < g_next_scan)
    {
        return false;
    }
    g_next_scan = now + UINT64_C(500000000);
    ++n_scans;
    FindRegions(dbg);
    ScanMessages(dbg);
    os::SleepThread(TimeSpan::FromMilliSeconds(60));
    for (u32 k = 0; k < g_nmsgs; ++k)
    {
        Msg m;
        if (!ReadMsg(dbg, k, std::addressof(m)) || m.fence_value == g_msgs[k].fence_value)
        {
            continue;
        }
        m.changed_ns = NowNs();
        g_msgs[k] = m;
        if (g_active < 0 && nxrp_present::FinishedSlot(m.slot, true) >= 0)
        {
            g_active = static_cast<s32>(k);
        }
    }
    if (n_scans <= 10 || g_active >= 0)
    {
        ams::mitm::applet::LogLine("layers: present scan %llu: %u region(s), %u message(s), %s",
                                   static_cast<unsigned long long>(n_scans), g_nregs, g_nmsgs,
                                   g_active >= 0 ? "HOME layer found" : "none moving yet");
        if (g_active >= 0)
        {
            ams::mitm::applet::LogLine("layers: following layer %#llx (syncpoint %u) at %#llx",
                                       static_cast<unsigned long long>(g_msgs[g_active].layer),
                                       g_msgs[g_active].fence_id, static_cast<unsigned long long>(g_msgs[g_active].va));
        }
    }
    g_seen_valid = false;
    return g_active >= 0;
}

// Another layer (a library applet) presenting while the followed one has gone quiet: follow it.
void SwitchIfOtherMoved(::ams::svc::Handle dbg, u64 now)
{
    for (u32 k = 0; k < g_nmsgs; ++k)
    {
        if (static_cast<s32>(k) == g_active)
        {
            continue;
        }
        Msg m;
        if (!ReadMsg(dbg, k, std::addressof(m)) || m.fence_value == g_msgs[k].fence_value)
        {
            continue;
        }
        m.changed_ns = now;
        g_msgs[k] = m;
        if (nxrp_present::FinishedSlot(m.slot, true) >= 0 && now - g_msgs[g_active].changed_ns > UINT64_C(60000000))
        {
            g_active = static_cast<s32>(k);
            g_seen_valid = false;
            ++n_switch;
            if (n_switch <= 16)
            {
                ams::mitm::applet::LogLine("layers: following layer %#llx (syncpoint %u)",
                                           static_cast<unsigned long long>(m.layer), m.fence_id);
            }
        }
    }
}

// Block until the followed layer makes a NEW present, polling its message. *idle: none for IdleNs (the display holds
// the newest slot). A change already there at the first poll has an unknown age (the display may have moved on and the
// other slot be redrawing), so it is skipped and the next present awaited.
bool WaitForPresent(::ams::svc::Handle dbg, Msg *out, bool *idle)
{
    const u64 t0 = NowNs();
    u64 last_all = t0;
    u32 polls = 0;
    bool first = true;
    for (;;)
    {
        const u64 now = NowNs();
        Msg m;
        if (!ReadMsg(dbg, static_cast<u32>(g_active), std::addressof(m)))
        {
            g_nmsgs = 0;
            g_active = -1;
            g_seen_valid = false;
            return false;
        }
        if (!g_seen_valid)
        {
            g_seen_val = m.fence_value;
            g_seen_valid = true;
        }
        else if (m.fence_value != g_seen_val)
        {
            m.changed_ns = now;
            g_msgs[g_active] = m;
            if (first)
            {
                ++n_unknown_age;
                g_seen_val = m.fence_value;
            }
            else
            {
                *out = m;
                *idle = false;
                return true;
            }
        }
        first = false;
        if (now - last_all >= UINT64_C(20000000))
        {
            last_all = now;
            SwitchIfOtherMoved(dbg, now);
            if (!g_seen_valid)
            {
                *out = m;
                return false;
            }
        }
        if (now - t0 >= IdleNs)
        {
            *out = m;
            *idle = true;
            return true;
        }
        if ((++polls & 15) == 0)
        {
            DrainContinue(dbg);
        }
        os::SleepThread(TimeSpan::FromMicroSeconds(500));
    }
}

// One short pass: the whole slot, raw, as 6 block-row reads.
bool FetchRaw(::ams::svc::Handle dbg, const Target &t, u32 slot, u8 *raw)
{
    const u64 slot_base = t.base + static_cast<u64>(slot) * SlotBytes;
    for (u32 br = 0; br < SlotRows / 128; ++br)
    {
        if (R_FAILED(::ams::svc::ReadDebugProcessMemory(reinterpret_cast<uintptr_t>(raw + br * BlockRowBytes), dbg,
                                                        slot_base + br * BlockRowBytes, BlockRowBytes)))
        {
            return false;
        }
    }
    return true;
}

// True when the live slot still equals the copy at 128 spread spots.
bool CopyStillMatches(::ams::svc::Handle dbg, const Target &t, u32 slot, const u8 *raw)
{
    u8 b[64];
    const u64 slot_base = t.base + static_cast<u64>(slot) * SlotBytes;
    for (u32 i = 0; i < PostSpots; ++i)
    {
        const u64 off = SpotOffset(i);
        if (R_FAILED(
                ::ams::svc::ReadDebugProcessMemory(reinterpret_cast<uintptr_t>(b), dbg, slot_base + off, sizeof(b))))
        {
            return false;
        }
        if (std::memcmp(b, raw + off, sizeof(b)) != 0)
        {
            return false;
        }
    }
    return true;
}

void DzMain(void *)
{
    for (;;)
    {
        os::WaitEvent(std::addressof(g_dz_go));
        nxrp_deswizzle::BlockRows(g_dz_raw, g_dz_dst, 0, DzSplit);
        os::SignalEvent(std::addressof(g_dz_done));
    }
}

bool DzEnsure()
{
    if (g_dz_state != 0)
    {
        return g_dz_state > 0;
    }
    os::InitializeEvent(std::addressof(g_dz_go), false, os::EventClearMode_AutoClear);
    os::InitializeEvent(std::addressof(g_dz_done), false, os::EventClearMode_AutoClear);
    if (R_FAILED(os::CreateThread(std::addressof(g_dz_thread), DzMain, nullptr, g_dz_stack, sizeof(g_dz_stack),
                                  os::GetThreadPriority(os::GetCurrentThread()))))
    {
        g_dz_state = -1;
        ams::mitm::applet::LogLine("layers: de-swizzle worker thread not created, single-thread copy");
        return false;
    }
    os::SetThreadNamePointer(std::addressof(g_dz_thread), "nxrp.layers.dz");
    os::StartThread(std::addressof(g_dz_thread));
    g_dz_state = 1;
    return true;
}

// raw slot copy -> dst, block-rows 0..2 on the worker, 3..5 here (one thread if the worker could not start). Returns
// the time taken.
u64 Deswizzle(const u8 *raw, u8 *dst)
{
    const u64 t0 = NowNs();
    if (DzEnsure())
    {
        g_dz_raw = raw;
        g_dz_dst = dst;
        os::SignalEvent(std::addressof(g_dz_go));
        nxrp_deswizzle::BlockRows(raw, dst, DzSplit, nxrp_deswizzle::BlockRowCount);
        os::WaitEvent(std::addressof(g_dz_done));
        ++n_dz_split;
    }
    else
    {
        nxrp_deswizzle::BlockRows(raw, dst, 0, nxrp_deswizzle::BlockRowCount);
    }
    return NowNs() - t0;
}

// vi base layer into dst. Returns false for "use the screenshot source this frame".
bool ReadViFrame(u8 *dst, size_t capacity, u64 *read_ns, u64 *att_ns, u64 *wait_ns, u64 *present_tick)
{
    if (capacity < RawNeed)
    {
        if (n_fail++ < 10)
        {
            ams::mitm::applet::LogLine("layers: capture region %zu B too small for the staging copy", capacity);
        }
        return false;
    }
    u8 *raw = dst + RawOffset;
    ::ams::svc::Handle dbg = ::ams::svc::InvalidHandle;
    const u64 t_att = NowNs();
    if (!Attach(g_vi, std::addressof(dbg)))
    {
        g_vi.ok = false;
        ++n_fail;
        if (n_fail <= 10)
        {
            ams::mitm::applet::LogLine("layers: attach to vi failed");
        }
        return false;
    }
    *att_ns += NowNs() - t_att;
    if (NowNs() - g_last_call_end > StaleCallNs)
    {
        g_seen_valid = false;
    }
    if (g_active < 0 && !FindActive(dbg))
    {
        Detach(dbg);
        ++n_fail;
        return false;
    }

    bool ok = false;
    Msg cur = {};
    bool idle = false;
    u32 slot = 0;
    for (u32 attempt = 0; attempt < 2 && !ok; ++attempt)
    {
        if (attempt == 0)
        {
            const u64 t_wait = NowNs();
            const bool got = WaitForPresent(dbg, std::addressof(cur), std::addressof(idle));
            *wait_ns += NowNs() - t_wait;
            if (!got)
            {
                break;
            }
            *present_tick = armGetSystemTick();
        }
        else
        {
            idle = false; // the newer present seen during the previous copy is fresh
            ++n_retry;
        }
        const int want = nxrp_present::FinishedSlot(cur.slot, idle);
        if (want < 0)
        {
            ++n_foreign;
            break;
        }
        slot = static_cast<u32>(want);
        const u64 t_raw = NowNs();
        if (!FetchRaw(dbg, g_vi, slot, raw))
        {
            g_vi.ok = false;
            ++n_fail;
            if (n_fail <= 10)
            {
                ams::mitm::applet::LogLine("layers: vi slot read failed");
            }
            break;
        }
        Msg after;
        const bool msg_ok = ReadMsg(dbg, static_cast<u32>(g_active), std::addressof(after));
        const bool unchanged = msg_ok && after.fence_value == cur.fence_value && CopyStillMatches(dbg, g_vi, slot, raw);
        const u64 raw_ns = NowNs() - t_raw;
        raw_sum += raw_ns;
        if (raw_ns > raw_max)
        {
            raw_max = raw_ns;
        }
        *read_ns += raw_ns;
        if (unchanged)
        {
            ok = true;
            break;
        }
        ++n_tear;
        if (!msg_ok)
        {
            break;
        }
        after.changed_ns = NowNs();
        g_msgs[g_active] = after;
        cur = after;
    }
    Detach(dbg);
    g_last_call_end = NowNs();
    if (!ok)
    {
        return false;
    }

    g_seen_val = cur.fence_value;
    if (idle)
    {
        ++n_idle;
    }
    else
    {
        ++n_synced;
    }
    const u64 dz_ns = Deswizzle(raw, dst);
    dz_sum += dz_ns;
    if (dz_ns > dz_max)
    {
        dz_max = dz_ns;
    }
    *read_ns += dz_ns;
    return true;
}

// Is an overlay (Quick Settings and the like) drawn right now? A brief attach to overlayDisp and a look at its newest
// slot. The overlay's own buffer cannot be copied reliably (- on the Lite: the panel showed flashing diagonal
// and striped garbage, and the same defects sit in the raw slot memory in the  dumps), so while an overlay is up
// the frame comes from the screenshot source, which the display hardware composes with the overlay included.
bool OverlayActive(u64 *att_ns)
{
    if (!g_ov.ok)
    {
        return false;
    }
    ::ams::svc::Handle odbg = ::ams::svc::InvalidHandle;
    const u64 t_att = NowNs();
    if (!Attach(g_ov, std::addressof(odbg)))
    {
        g_ov.ok = false;
        return false;
    }
    *att_ns += NowNs() - t_att;
    u32 slot = 0;
    bool active = false;
    if (!PickOverlaySlot(odbg, std::addressof(slot), std::addressof(active)))
    {
        g_ov.ok = false;
        active = false;
    }
    Detach(odbg);
    return active;
}

} // namespace

extern "C" bool UnifiedLayersEligible(void)
{
    if (!g_logged_armed)
    {
        g_logged_armed = true;
        ams::mitm::applet::LogLine("layers: armed (present-synced read)");
    }
    if (ams::mitm::applet::g_pmdmnt_rc != 0)
    {
        return false;
    }
    os::ProcessId app{};
    if (R_SUCCEEDED(::ams::pm::dmnt::GetApplicationProcessId(std::addressof(app))))
    {
        return false;
    }
    // The Album or hbmenu draws on its own layer, which following HOME's layer cannot see.
    os::ProcessId album{};
    return R_FAILED(::ams::pm::dmnt::GetProcessId(std::addressof(album),
                                                  ::ams::ncm::ProgramId{UINT64_C(0x010000000000100D)})) ||
           album.value == 0;
}

extern "C" void UnifiedLayersNoteFallback(void)
{
    if (g_ov_shot_pending)
    {
        g_ov_shot_pending = false;
        ++n_ov_shot;
        if (n_ov_shot == 1)
        {
            ams::mitm::applet::LogLine("layers: an overlay is up, using the screenshot source until it is gone");
        }
        return;
    }
    ++n_fallback;
    if (n_fallback <= 10 || n_fallback % 120 == 0)
    {
        ams::mitm::applet::LogLine("layers: failed, using the screenshot source for this frame (fallbacks %llu)",
                                   static_cast<unsigned long long>(n_fallback));
    }
}

extern "C" bool UnifiedLayersReadFrame(void *pixels, size_t capacity, uint64_t *elapsed_ns, uint64_t *capture_tick)
{
    if (pixels == nullptr || capacity < size_t(W) * H * 4 || elapsed_ns == nullptr || capture_tick == nullptr)
    {
        return false;
    }
    const u64 t_begin = NowNs();
    if (!g_vi.ok || (!g_ov.ok && t_begin >= g_next_resolve))
    {
        const bool a = Resolve(g_vi);
        static_cast<void>(Resolve(g_ov));
        g_next_resolve = t_begin + UINT64_C(2000000000);
        if (!a)
        {
            ++n_fail;
            if (n_fail <= 10)
            {
                ams::mitm::applet::LogLine("layers: vi slot region not found");
            }
            return false;
        }
        ams::mitm::applet::LogLine(
            "layers: vi pid %llu %llu KB (%u slots); overlayDisp %s", static_cast<unsigned long long>(g_vi.pid),
            static_cast<unsigned long long>(g_vi.size / 1024), g_vi.slots, g_ov.ok ? "found" : "not found");
    }
    u8 *dst = static_cast<u8 *>(pixels);
    u64 read_ns = 0;
    u64 att_ns = 0;
    u64 wait_ns = 0;
    u64 present_tick = 0;

    // overlay up (or just was): the screenshot source composes it; the base-layer follower restarts clean afterwards
    if (OverlayActive(std::addressof(att_ns)))
    {
        ++n_ov_active;
        g_ov_hold_until = NowNs() + OverlayHoldNs;
    }
    if (NowNs() < g_ov_hold_until)
    {
        g_seen_valid = false;
        g_ov_shot_pending = true;
        return false;
    }

    // base layer: vi, in step with HOME's presents (copy has alpha 255; the encoder needs an opaque frame)
    if (!ReadViFrame(dst, capacity, std::addressof(read_ns), std::addressof(att_ns), std::addressof(wait_ns),
                     std::addressof(present_tick)))
    {
        return false;
    }

    armDCacheFlush(pixels, size_t(W) * H * 4);

    const u64 total = NowNs() - t_begin - wait_ns; // the wait for HOME's next present is pacing, not source cost
    ++n_frames;
    att_sum += att_ns;
    if (att_ns > att_max)
    {
        att_max = att_ns;
    }
    rd_sum += read_ns;
    if (read_ns > rd_max)
    {
        rd_max = read_ns;
    }
    tot_sum += total;
    if (total > tot_max)
    {
        tot_max = total;
    }
    wait_sum += wait_ns;
    if (wait_ns > wait_max)
    {
        wait_max = wait_ns;
    }
    if (n_frames % 240 == 0)
    {
        ams::mitm::applet::LogLine(
            "layers: %llu frames (synced %llu, idle %llu, retried %llu, unknown-age skips %llu, foreign slot %llu); copy+deswizzle avg %llu max %llu us; deswizzle avg %llu max %llu us (split %llu); raw copy avg %llu max %llu us; wait avg %llu max %llu us; attach avg %llu max %llu us; total avg %llu max %llu us; overlay seen %llu (frames from the screenshot source while it was up %llu); unstable overlay picks %llu; vi copies discarded %llu; scans %llu; failures %llu; fallbacks %llu",
            static_cast<unsigned long long>(n_frames), static_cast<unsigned long long>(n_synced),
            static_cast<unsigned long long>(n_idle), static_cast<unsigned long long>(n_retry),
            static_cast<unsigned long long>(n_unknown_age), static_cast<unsigned long long>(n_foreign),
            static_cast<unsigned long long>(rd_sum / n_frames / 1000), static_cast<unsigned long long>(rd_max / 1000),
            static_cast<unsigned long long>(dz_sum / n_frames / 1000), static_cast<unsigned long long>(dz_max / 1000),
            static_cast<unsigned long long>(n_dz_split), static_cast<unsigned long long>(raw_sum / n_frames / 1000),
            static_cast<unsigned long long>(raw_max / 1000),
            static_cast<unsigned long long>(wait_sum / n_frames / 1000),
            static_cast<unsigned long long>(wait_max / 1000),
            static_cast<unsigned long long>(att_sum / n_frames / 1000), static_cast<unsigned long long>(att_max / 1000),
            static_cast<unsigned long long>(tot_sum / n_frames / 1000), static_cast<unsigned long long>(tot_max / 1000),
            static_cast<unsigned long long>(n_ov_active), static_cast<unsigned long long>(n_ov_shot),
            static_cast<unsigned long long>(n_unstable), static_cast<unsigned long long>(n_tear),
            static_cast<unsigned long long>(n_scans), static_cast<unsigned long long>(n_fail),
            static_cast<unsigned long long>(n_fallback));
    }
    *elapsed_ns = total;
    *capture_tick = present_tick != 0 ? present_tick : armGetSystemTick();
    return true;
}
