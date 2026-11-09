#pragma once
#include <stdbool.h>
#include <stdint.h>

// Game audio for clients that ask for it (FeatureFlags bit ExtraFeatureFlags_OpusAudio): 48 kHz stereo Opus, 20 ms
// frames, about 128 kbps, in place of 1.536 Mbps of raw PCM. One encoder per viewer.
typedef struct OpusAudio OpusAudio;

OpusAudio *OpusAudio_Create(void);
void OpusAudio_Destroy(OpusAudio *enc);
// Encodes one frame of PCM_FRAME_BYTES bytes. Returns the packet size, or 0 on failure.
uint32_t OpusAudio_Encode(OpusAudio *enc, const uint8_t *pcm, uint8_t *out, uint32_t outMax);
