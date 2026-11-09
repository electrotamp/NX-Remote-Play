#pragma once
#include <stdint.h>
#include <string.h>

// Gathers the 4 KiB audio chunks of the capture ring into whole Opus frames (20 ms of 48 kHz stereo 16-bit =
// 3840 bytes). Whatever does not fill a frame stays for the next call.
#define PCM_FRAME_BYTES 3840u
#define PCM_ACCUM_BYTES (PCM_FRAME_BYTES + 0x1000u * 6u)

typedef struct
{
    uint8_t data[PCM_ACCUM_BYTES];
    uint32_t fill;
} PcmAccum;

static inline void PcmAccumReset(PcmAccum *a)
{
    a->fill = 0;
}

// Appends a chunk. A chunk that cannot fit (it would only happen if frames were never taken) is cut, never overrun.
static inline void PcmAccumAdd(PcmAccum *a, const uint8_t *pcm, uint32_t size)
{
    if (size > PCM_ACCUM_BYTES - a->fill)
        size = PCM_ACCUM_BYTES - a->fill;
    memcpy(a->data + a->fill, pcm, size);
    a->fill += size;
}

static inline int PcmAccumHasFrame(const PcmAccum *a)
{
    return a->fill >= PCM_FRAME_BYTES;
}

// The oldest whole frame, or NULL; PcmAccumPop removes it.
static inline const uint8_t *PcmAccumFrame(const PcmAccum *a)
{
    return PcmAccumHasFrame(a) ? a->data : 0;
}

static inline void PcmAccumPop(PcmAccum *a)
{
    if (a->fill < PCM_FRAME_BYTES)
        return;
    a->fill -= PCM_FRAME_BYTES;
    memmove(a->data, a->data + PCM_FRAME_BYTES, a->fill);
}
