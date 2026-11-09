#pragma once
#include <cstdint>

// Focus alone stayed "1" on the Lite after HOME stopped the game's presents.
// Require both observations. A low-rate/idle application uses the composed
// source instead; this is not a complete overlay visibility detector.
inline bool HybridUseGame(uint64_t app, uint64_t observed, uint32_t state, uint64_t focus_age_ns, bool has_present,
                          uint64_t present_age_ns)
{
    return app != 0 && app == observed && state == 1 && focus_age_ns < 300000000ULL && has_present &&
           present_age_ns < 250000000ULL;
}
