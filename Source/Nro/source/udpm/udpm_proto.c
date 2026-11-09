#include "udpm_proto.h"
#include <string.h>

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}
static void put64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}
static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t get32(const uint8_t *p)
{
    uint32_t v = 0;
    for (int i = 3; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}
static uint64_t get64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

static void putPrefix(uint8_t *out, uint8_t type, uint8_t flags, uint32_t session)
{
    out[0] = UDPM_MAGIC;
    out[1] = UDPM_VERSION;
    out[2] = type;
    out[3] = flags;
    put32(out + 4, session);
}

static int checkPrefix(const uint8_t *in, size_t len, size_t want, uint8_t type, uint32_t session)
{
    return len == want && in[0] == UDPM_MAGIC && in[1] == UDPM_VERSION && in[2] == type && get32(in + 4) == session;
}

int UdpmPeekType(const uint8_t *in, size_t len)
{
    if (len < 8 || in[0] != UDPM_MAGIC || in[1] != UDPM_VERSION)
        return 0;
    return in[2];
}

size_t UdpmWriteShardHeader(uint8_t *out, const UdpmShardHeader *h)
{
    putPrefix(out, h->type, h->flags, h->session);
    put32(out + 8, h->packet_seq);
    put32(out + 12, h->frame_id);
    put32(out + 16, h->frame_size);
    out[20] = h->block_index;
    out[21] = h->block_count;
    out[22] = h->shard_index;
    out[23] = h->data_shards;
    out[24] = h->parity_shards;
    out[25] = 0;
    put16(out + 26, h->shard_size);
    put64(out + 28, h->timestamp_us);
    return UDPM_HEADER_SIZE;
}

int UdpmReadShardHeader(const uint8_t *in, size_t len, UdpmShardHeader *h)
{
    if (len < UDPM_HEADER_SIZE || in[0] != UDPM_MAGIC || in[1] != UDPM_VERSION)
        return 0;
    const uint8_t type = in[2];
    if (type != UdpmType_Video && type != UdpmType_Audio && type != UdpmType_Probe)
        return 0;
    h->type = type;
    h->flags = in[3];
    h->session = get32(in + 4);
    h->packet_seq = get32(in + 8);
    h->frame_id = get32(in + 12);
    h->frame_size = get32(in + 16);
    h->block_index = in[20];
    h->block_count = in[21];
    h->shard_index = in[22];
    h->data_shards = in[23];
    h->parity_shards = in[24];
    h->shard_size = get16(in + 26);
    h->timestamp_us = get64(in + 28);
    const unsigned total = (unsigned)h->data_shards + h->parity_shards;
    if (h->block_count == 0 || h->block_index >= h->block_count)
        return 0;
    if (h->data_shards == 0 || h->data_shards > UDPM_MAX_DATA_SHARDS || h->parity_shards > UDPM_MAX_PARITY_SHARDS)
        return 0;
    if (h->shard_index >= total || h->shard_size == 0 || h->shard_size > UDPM_MAX_SHARD_SIZE)
        return 0;
    if (len != (size_t)UDPM_HEADER_SIZE + h->shard_size)
        return 0;
    if (((h->flags & UdpmFlag_Parity) != 0) != (h->shard_index >= h->data_shards))
        return 0;
    if (h->frame_size == 0 || h->frame_size > (uint32_t)h->block_count * UDPM_MAX_DATA_SHARDS * h->shard_size)
        return 0;
    return 1;
}

size_t UdpmWriteNack(uint8_t *out, uint32_t session, const UdpmNack *n)
{
    putPrefix(out, UdpmType_Nack, 0, session);
    put32(out + 8, n->frame_id);
    out[12] = n->block_index;
    out[13] = n->stream_type;
    put16(out + 14, 0);
    put64(out + 16, n->missing_lo);
    put64(out + 24, n->missing_hi);
    return UDPM_NACK_SIZE;
}

int UdpmReadNack(const uint8_t *in, size_t len, uint32_t session, UdpmNack *n)
{
    if (!checkPrefix(in, len, UDPM_NACK_SIZE, UdpmType_Nack, session))
        return 0;
    n->frame_id = get32(in + 8);
    n->block_index = in[12];
    n->stream_type = in[13];
    n->missing_lo = get64(in + 16);
    n->missing_hi = get64(in + 24);
    return n->missing_lo != 0 || n->missing_hi != 0;
}

size_t UdpmWriteFeedback(uint8_t *out, uint32_t session, const UdpmFeedback *f)
{
    putPrefix(out, UdpmType_Feedback, 0, session);
    const uint32_t v[8] = {f->seq,          f->highest_frame_id,     f->packets_received,
                           f->packets_lost, f->shards_recovered_fec, f->frames_dropped,
                           f->jitter_us,    f->playout_delay_us};
    for (int i = 0; i < 8; i++)
        put32(out + 8 + 4 * i, v[i]);
    return UDPM_FEEDBACK_SIZE;
}

size_t UdpmWriteCongestion(uint8_t *out, uint32_t session, const UdpmCongestion *c)
{
    putPrefix(out, UdpmType_Congestion, 0, session);
    const uint32_t v[8] = {c->seq,           c->interval_ms, c->frames,          c->excess_min_us,
                           c->excess_max_us, c->nacks,       c->audio_underruns, c->reserved};
    for (int i = 0; i < 8; i++)
        put32(out + 8 + 4 * i, v[i]);
    return UDPM_CONGESTION_SIZE;
}

int UdpmReadFeedback(const uint8_t *in, size_t len, uint32_t session, UdpmFeedback *f)
{
    if (!checkPrefix(in, len, UDPM_FEEDBACK_SIZE, UdpmType_Feedback, session))
        return 0;
    uint32_t *v[8] = {&f->seq,          &f->highest_frame_id,     &f->packets_received,
                      &f->packets_lost, &f->shards_recovered_fec, &f->frames_dropped,
                      &f->jitter_us,    &f->playout_delay_us};
    for (int i = 0; i < 8; i++)
        *v[i] = get32(in + 8 + 4 * i);
    return 1;
}

size_t UdpmWriteKeyframeRequest(uint8_t *out, uint32_t session, uint32_t last_good_frame_id)
{
    putPrefix(out, UdpmType_KeyframeRequest, 0, session);
    put32(out + 8, last_good_frame_id);
    return UDPM_KEYFRAME_REQUEST_SIZE;
}

int UdpmReadKeyframeRequest(const uint8_t *in, size_t len, uint32_t session, uint32_t *last_good_frame_id)
{
    if (!checkPrefix(in, len, UDPM_KEYFRAME_REQUEST_SIZE, UdpmType_KeyframeRequest, session))
        return 0;
    *last_good_frame_id = get32(in + 8);
    return 1;
}

size_t UdpmWriteHello(uint8_t *out, uint32_t session, uint32_t nonce)
{
    putPrefix(out, UdpmType_Hello, 0, session);
    put32(out + 8, nonce);
    return UDPM_HELLO_SIZE;
}

int UdpmReadHello(const uint8_t *in, size_t len, uint32_t session, uint32_t *nonce)
{
    if (!checkPrefix(in, len, UDPM_HELLO_SIZE, UdpmType_Hello, session))
        return 0;
    *nonce = get32(in + 8);
    return 1;
}
