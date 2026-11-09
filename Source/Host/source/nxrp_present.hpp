// SPDX-License-Identifier: GPL-2.0-only
//  pure helpers for the layers source's present-synced HOME read. No Atmosphere/libnx dependency so the
// host tests can compile this file. The message layout is the reference project's  finding (the reference notes
// 7): vi keeps the last PresentSharedFrameBuffer request in an IPC buffer: "SFCI" at -0x58 and command 8255 at
// -0x50 (or "SFCO" and result 0 once vi has answered; the arguments stay), fence {count, {id, value} x4} at -0x48, swap
// interval at -0x10, pad at -0x0C, layer at -0x08, slot word at +0.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace nxrp_present
{

constexpr uint32_t MagicSfci = 0x49434653, MagicSfco = 0x4f434653;
constexpr size_t Lead = 0x58;   // bytes before the slot word that Parse() reads
constexpr size_t Window = 0x60; // Lead + the slot word (8) rounded up
constexpr uint32_t NumSlots = 7;

struct Msg
{
    uint64_t va;
    uint64_t layer;
    uint32_t fence_id, fence_value;
    int32_t slot;
    uint64_t changed_ns;
};

// p points at the slot word; Lead bytes before it must be readable. False for anything that is not a real present.
inline bool Parse(const uint8_t *p, Msg *m)
{
    uint32_t cnt, id, val, swap, pad, magic, cmd;
    uint64_t layer, slot;
    std::memcpy(&magic, p - 0x58, 4);
    std::memcpy(&cmd, p - 0x50, 4);
    if (!((magic == MagicSfci && cmd == 8255) || (magic == MagicSfco && cmd == 0)))
    {
        return false;
    }
    std::memcpy(&cnt, p - 0x48, 4);
    std::memcpy(&id, p - 0x44, 4);
    std::memcpy(&val, p - 0x40, 4);
    std::memcpy(&swap, p - 0x10, 4);
    std::memcpy(&pad, p - 0x0C, 4);
    std::memcpy(&layer, p - 0x08, 8);
    std::memcpy(&slot, p, 8);
    if (cnt < 1 || cnt > 4 || id == 0 || id >= 192 || swap > 4 || pad != 0)
    {
        return false;
    }
    if (layer == 0 || layer >= 0x1000 || slot >= NumSlots)
    {
        return false;
    }
    m->layer = layer;
    m->fence_id = id;
    m->fence_value = val;
    m->slot = static_cast<int32_t>(slot);
    return true;
}

// HOME / System Settings / library applets double-buffer in vi slots 3 and 4. `newest` is the slot of the newest
// present. While that present is being drawn or waiting for the display, the OTHER slot is the one on screen and
// finished; once the display has held `newest` for a while with no further present (idle), `newest` itself is the
// finished one. -1: not a slot pair this source can follow.
inline int FinishedSlot(int newest, bool idle)
{
    if (newest != 3 && newest != 4)
    {
        return -1;
    }
    return idle ? newest : (newest == 3 ? 4 : 3);
}

} // namespace nxrp_present
