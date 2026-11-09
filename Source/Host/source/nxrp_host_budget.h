#pragma once
#include <stdint.h>

//  offered-video budget only. Actual pacing/receiver feedback is separate.
// Three direct viewers share one encode, but each consumes a network copy.
static inline uint32_t NxrpHostVideoBudgetBps(uint32_t viewers, uint32_t audio_viewers)
{
    if (!viewers || viewers > 3 || audio_viewers > viewers)
        return 0;
    const uint32_t payload = 10000000u * 9u / 10u;
    const uint32_t audio = audio_viewers * 1536000u;
    return (payload - audio) / viewers;
}
