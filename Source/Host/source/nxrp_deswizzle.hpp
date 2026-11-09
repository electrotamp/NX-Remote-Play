// SPDX-License-Identifier: GPL-2.0-only
//  pure block-linear -> pitch-linear copy for the vi base layer (opaque RGBA, 1280x720 out of a
// 1280x768 slot). Layout (/, tap deswizzle.py): GOB = 64 B x 8 rows = 512 B; block = 16 GOBs stacked = 64 B x
// 128 rows = 8192 B; 80 blocks per block-row; 6 block-rows per slot. Inside a GOB a 64-byte cache line at `gob + 64*q`
// holds, for the row pair (2q, 2q+1) and x 0..31: [row 2q x0-15][row 2q+1 x0-15][row 2q x16-31][row 2q+1 x16-31]; the
// line at `gob + 256 + 64*q` holds the same for x 32..63. Reading every line once and writing both rows from it
// replaces 's per-16-byte offset arithmetic.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace nxrp_deswizzle
{

constexpr uint32_t Width = 1280, Height = 720, SlotRows = 768;
constexpr size_t Pitch = size_t(Width) * 4;
constexpr uint32_t BlocksPerRow = 80, BlockBytes = 8192, GobBytes = 512;
constexpr size_t BlockRowBytes = size_t(BlocksPerRow) * BlockBytes; // 655,360
constexpr uint32_t BlockRowCount = SlotRows / 128;                  // 6

// 16 bytes (4 RGBA pixels), alpha forced 255.
inline void Copy16Opaque(uint8_t *d, const uint8_t *s)
{
    uint64_t a, b;
    std::memcpy(&a, s, 8);
    std::memcpy(&b, s + 8, 8);
    a |= UINT64_C(0xFF000000FF000000);
    b |= UINT64_C(0xFF000000FF000000);
    std::memcpy(d, &a, 8);
    std::memcpy(d + 8, &b, 8);
}

// Block-rows [br_begin, br_end) of the raw slot into dst, highest block-row first (the last bytes read are the warmest
// in cache). Opaque copy: alpha is forced to 255 (the encoder needs an opaque frame).
inline void BlockRows(const uint8_t *raw, uint8_t *dst, uint32_t br_begin, uint32_t br_end)
{
    for (uint32_t br = br_end; br-- > br_begin;)
    {
        for (uint32_t bx = 0; bx < BlocksPerRow; ++bx)
        {
            const uint8_t *blk = raw + size_t(br) * BlockRowBytes + size_t(bx) * BlockBytes;
            for (uint32_t gy = 0; gy < 16; ++gy)
            {
                const uint32_t y = br * 128 + gy * 8;
                if (y >= Height)
                {
                    break;
                }
                const uint8_t *gob = blk + size_t(gy) * GobBytes;
                uint8_t *d0 = dst + size_t(y) * Pitch + size_t(bx) * 64;
                for (uint32_t q = 0; q < 4; ++q, d0 += 2 * Pitch)
                {
                    const uint8_t *l0 = gob + q * 64, *l1 = gob + 256 + q * 64;
                    uint8_t *d1 = d0 + Pitch;
                    __builtin_prefetch(l0 + GobBytes);
                    __builtin_prefetch(l1 + GobBytes);
                    Copy16Opaque(d0 + 0, l0 + 0);
                    Copy16Opaque(d1 + 0, l0 + 16);
                    Copy16Opaque(d0 + 16, l0 + 32);
                    Copy16Opaque(d1 + 16, l0 + 48);
                    Copy16Opaque(d0 + 32, l1 + 0);
                    Copy16Opaque(d1 + 32, l1 + 16);
                    Copy16Opaque(d0 + 48, l1 + 32);
                    Copy16Opaque(d1 + 48, l1 + 48);
                }
            }
        }
    }
}
// Reference ( / tap deswizzle.py gob_offset arithmetic), used by the host test only.

// Opaque copy, alpha forced 255 (what the host does).


} // namespace nxrp_deswizzle
