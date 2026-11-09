#include "OpusAudio.h"
#include <stdlib.h>
#include <opus/opus.h>
#include "pcm_accum.h"
#include "../core.h"

#define OPUS_RATE 48000
#define OPUS_CHANNELS 2
#define OPUS_FRAME_SAMPLES (PCM_FRAME_BYTES / (OPUS_CHANNELS * 2))
#define OPUS_BITRATE 128000

struct OpusAudio
{
    OpusEncoder *state;
};

OpusAudio *OpusAudio_Create(void)
{
    OpusAudio *enc = (OpusAudio *)calloc(1, sizeof(*enc));
    if (!enc)
        return NULL;
    int err = 0;
    enc->state = opus_encoder_create(OPUS_RATE, OPUS_CHANNELS, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
    if (!enc->state || err != OPUS_OK)
    {
        free(enc);
        return NULL;
    }
    opus_encoder_ctl(enc->state, OPUS_SET_BITRATE(OPUS_BITRATE));
    opus_encoder_ctl(enc->state, OPUS_SET_VBR(1));
    opus_encoder_ctl(enc->state, OPUS_SET_VBR_CONSTRAINT(1));
    opus_encoder_ctl(enc->state, OPUS_SET_COMPLEXITY(5));
    return enc;
}

void OpusAudio_Destroy(OpusAudio *enc)
{
    if (!enc)
        return;
    opus_encoder_destroy(enc->state);
    free(enc);
}

uint32_t OpusAudio_Encode(OpusAudio *enc, const uint8_t *pcm, uint8_t *out, uint32_t outMax)
{
    int16_t samples[OPUS_FRAME_SAMPLES * OPUS_CHANNELS];
    memcpy(samples, pcm, sizeof(samples)); // the ring bytes are not aligned for 16-bit reads
    const opus_int32 n = opus_encode(enc->state, samples, OPUS_FRAME_SAMPLES, out, (opus_int32)outMax);
    return n > 0 ? (uint32_t)n : 0;
}
