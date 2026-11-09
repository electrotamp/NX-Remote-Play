#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C"
{
#endif

    // NXRP  layers source: assemble the final 1280x720 frame (pitch-linear RGBA, bytes R,G,B,A,
    // alpha forced 255) from vi's shared frame buffer (HOME/Settings) plus overlayDisp's RGBA layer
    // (Quick Settings), both read by a brief debug attach. See nxrp_layers.cpp.
    // Eligible only when no application process is running.
    // the vi layer is read in step with HOME's own presents, so ReadFrame blocks up to ~50 ms for the next
    // present; `capacity` must be >= 8,126,464 (the raw slot copy is staged at +4 MiB, inside the capture region the
    // composed source does not use).
    bool UnifiedLayersEligible(void);
    bool UnifiedLayersReadFrame(void *pixels, size_t capacity, uint64_t *elapsed_ns, uint64_t *capture_tick);
    void UnifiedLayersNoteFallback(void);

#ifdef __cplusplus
}
#endif
