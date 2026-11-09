#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C"
{
#endif
    bool NxrpReadFrame(void *pixels, size_t capacity, uint64_t *elapsed_ns, uint64_t *capture_tick);
    bool NxrpSourceDone(void);
    uint32_t NxrpSourceState(void);
    void NxrpGameFrameCaptured(uint32_t skipped_presents);
    void NxrpSourceReset(void);
    bool NxrpGateBusy(void);
#ifdef __cplusplus
}
#endif
