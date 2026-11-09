#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C"
{
#endif
    bool CaptureVideoViewerPresent(void);
    bool CaptureVideoCanProduce(void);
    bool CaptureVideoUsesDirectDelivery(void);
    bool CaptureVideoWaitDelivery(void);
    uint32_t CaptureVideoBudgetBps(void);
    bool CaptureVideoSubmit(const void *data, size_t size, uint64_t timestamp_us);
    void NxrpVideo_Start(void);
    void NxrpVideo_RequestKeyframe(void);
    uint32_t NxrpVideo_SourceEpoch(void);
#ifdef __cplusplus
}
#endif

// The reference project sends complete Annex-B access units, including SPS/PPS before the slice.
// GRC's single-NAL packets also pass through this parser.
static inline bool CaptureVideoHasIdr(const uint8_t *data, size_t size)
{
    if (!data)
        return false;
    for (size_t i = 0; i + 3 < size; ++i)
    {
        if (data[i] || data[i + 1])
            continue;
        size_t n = i + 2;
        if (data[n] == 0)
            ++n;
        if (n + 1 < size && data[n] == 1 && (data[n + 1] & 31) == 5)
            return true;
    }
    return false;
}
