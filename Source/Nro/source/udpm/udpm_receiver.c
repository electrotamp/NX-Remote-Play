// Heap-based receiver, enabled for the NRO with UDPM_RECEIVER_HEAP. Never enable
// this in the heapless host sysmodule. Canonical-layout/window bounds are NRO additions.
#include "udpm_receiver.h"
#include "udpm_fec.h"
#include <stdlib.h>
#include <string.h>

static int32_t seqDiff(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b);
}

int UdpmReceiverInit(UdpmReceiver *r, uint8_t type, uint32_t session, UdpmFrameFn on_frame, void *ctx)
{
    memset(r, 0, sizeof(*r));
    r->type = type;
    r->session = session;
    r->on_frame = on_frame;
    r->ctx = ctx;
    r->hold_us = 40000;
    r->nack_after_us = 8000;
    r->waiting_keyframe = type == UdpmType_Video; // a stream starts decodable only at a keyframe
    r->frames = (UdpmRxFrame *)calloc(UDPM_RX_WINDOW, sizeof(UdpmRxFrame));
    UdpmGfInit();
    return r->frames != NULL;
}

static void freeFrame(UdpmRxFrame *f)
{
    for (unsigned b = 0; b < f->block_count; b++)
        free(f->blocks[b].buf);
    memset(f, 0, sizeof(*f));
}

void UdpmReceiverFree(UdpmReceiver *r)
{
    if (r->frames)
        for (unsigned i = 0; i < UDPM_RX_WINDOW; i++)
            freeFrame(&r->frames[i]);
    free(r->frames);
    free(r->assembly);
    memset(r, 0, sizeof(*r));
}

static UdpmRxFrame *slotFor(UdpmReceiver *r, uint32_t id)
{
    return &r->frames[id % UDPM_RX_WINDOW];
}

// Whether this shard's block fits the frame. full_k is the data-shard count of the non-last blocks
// when already known (0 if not): then the last block's size follows from the frame size.
static int layoutOk(const UdpmShardHeader *h, unsigned full_k)
{
    const uint32_t shard = h->shard_size;
    if (h->block_index + 1 < h->block_count)
    {
        const uint32_t span = (uint32_t)h->data_shards * shard;
        if (full_k && full_k != h->data_shards)
            return 0;
        return h->frame_size > (uint32_t)(h->block_count - 1) * span && h->frame_size <= (uint32_t)h->block_count * span;
    }
    if (h->block_count == 1)
        return h->data_shards == (h->frame_size + shard - 1) / shard;
    if (!full_k)
        return 1; // checked when a full block shows the layout
    const uint32_t before = (uint32_t)(h->block_count - 1) * full_k * shard;
    return h->frame_size > before && h->data_shards == (h->frame_size - before + shard - 1) / shard;
}

static void tryComplete(UdpmReceiver *r, UdpmRxFrame *f, UdpmRxBlock *b, uint64_t now_us)
{
    if (b->done || b->have < b->k)
        return;
    uint8_t *shards[UDPM_MAX_DATA_SHARDS + UDPM_MAX_PARITY_SHARDS];
    for (unsigned i = 0; i < (unsigned)b->k + b->m; i++)
        shards[i] = b->buf + (size_t)i * b->shard_size;
    const int rebuilt = UdpmFecDecode(shards, b->present, b->k, b->m, b->shard_size);
    if (rebuilt < 0)
    {
        return;
    }
    r->stats.shards_recovered_fec += (uint32_t)rebuilt;
    b->done = 1;
    if (++f->blocks_done == f->block_count)
        f->complete_us = now_us;
}

int UdpmReceiverPush(UdpmReceiver *r, const uint8_t *dg, size_t len, uint64_t now_us)
{
    UdpmShardHeader h;
    if (!UdpmReadShardHeader(dg, len, &h) || h.type != r->type || h.session != r->session)
        return 0;
    // Senders split a frame into blocks of a fixed number of data shards (the host and desktop use 32,
    // not UDPM_MAX_DATA_SHARDS); only the last block is shorter. Reject inconsistent layouts before
    // allocation/copy; bound the window to original NxRemotePlay packets.
    if (h.frame_size > 0x54000u + 18u || h.block_count == 0 || h.block_index >= h.block_count)
        return 0;
    UdpmRxFrame *slot = slotFor(r, h.frame_id);
    const uint8_t known_k = slot->used && slot->frame_id == h.frame_id ? slot->full_k : 0;
    if (!layoutOk(&h, known_k))
        return 0;
    if (!r->started)
    {
        r->next_id = h.frame_id;
        r->started = 1;
    }
    // Loss statistics count every valid datagram, including parity that arrives after its frame
    // was already delivered; only exact repeats of a shard we still track are taken back out.
    if (!r->stats.seq_seen)
    {
        r->stats.first_seq = r->stats.highest_seq = h.packet_seq;
        r->stats.seq_seen = 1;
    }
    else if (seqDiff(h.packet_seq, r->stats.highest_seq) > 0)
        r->stats.highest_seq = h.packet_seq;
    r->stats.packets_received++;
    if (seqDiff(h.frame_id, r->next_id) < 0)
        return 0; // frame already delivered/dropped
    if (seqDiff(h.frame_id, r->next_id) >= UDPM_RX_WINDOW)
    { // far ahead: we fell behind
        // Bound work to the actual window, even after a huge sequence jump.
        for (unsigned i = 0; i < UDPM_RX_WINDOW; ++i)
            if (r->frames[i].used)
                freeFrame(&r->frames[i]);
        r->stats.frames_dropped += h.frame_id - r->next_id;
        r->waiting_keyframe = r->type == UdpmType_Video;
        r->next_id = h.frame_id;
    }

    UdpmRxFrame *f = slotFor(r, h.frame_id);
    if (f->used && f->frame_id != h.frame_id)
        freeFrame(f); // stale occupant (already accounted)
    if (!f->used)
    {
        f->used = 1;
        f->frame_id = h.frame_id;
        f->frame_size = h.frame_size;
        f->block_count = h.block_count;
        f->flags = h.flags & UdpmFlag_Keyframe;
        f->timestamp_us = h.timestamp_us;
        // Jitter: variation of (arrival - send) between consecutive new frames.
        const int64_t transit = (int64_t)now_us - (int64_t)h.timestamp_us;
        if (r->transit_init)
        {
            int64_t d = transit - r->transit_prev;
            if (d < 0)
                d = -d;
            r->stats.jitter_us += (uint32_t)((d - (int64_t)r->stats.jitter_us) / 16);
        }
        r->transit_prev = transit;
        r->transit_init = 1;
    }
    if (f->frame_size != h.frame_size || f->block_count != h.block_count)
        return 0;
    if (f->timestamp_us != h.timestamp_us || f->flags != (h.flags & UdpmFlag_Keyframe))
        return 0;
    for (unsigned bi = 0; bi < f->block_count; ++bi)
        if (f->blocks[bi].buf && f->blocks[bi].shard_size != h.shard_size)
            return 0;

    if (h.block_index + 1 < h.block_count && !f->full_k)
    {
        // First full block of this frame: the last block (if it came earlier) must fit this layout.
        UdpmRxBlock *last = &f->blocks[h.block_count - 1];
        f->full_k = h.data_shards;
        if (last->buf)
        {
            UdpmShardHeader lh = h;
            lh.block_index = h.block_count - 1;
            lh.data_shards = last->k;
            if (!layoutOk(&lh, f->full_k))
            {
                f->full_k = 0;
                return 0;
            }
        }
    }

    UdpmRxBlock *b = &f->blocks[h.block_index];
    if (!b->buf)
    {
        b->k = h.data_shards;
        b->m = h.parity_shards;
        b->shard_size = h.shard_size;
        b->last_us = now_us;
        b->buf = (uint8_t *)malloc((size_t)(b->k + b->m) * b->shard_size);
        if (!b->buf)
            return 0;
    }
    if (b->k != h.data_shards || b->m != h.parity_shards || b->shard_size != h.shard_size)
        return 0;
    if (b->present[h.shard_index])
    {
        r->stats.packets_received--;
        return 1;
    }
    b->present[h.shard_index] = 1;
    b->last_us = now_us;
    if (b->done)
        return 1;
    memcpy(b->buf + (size_t)h.shard_index * b->shard_size, dg + UDPM_HEADER_SIZE, b->shard_size);
    b->have++;
    tryComplete(r, f, b, now_us);
    UdpmReceiverPoll(r, now_us);
    return 1;
}

static void deliver(UdpmReceiver *r, UdpmRxFrame *f)
{
    const int key = (f->flags & UdpmFlag_Keyframe) != 0;
    if (r->waiting_keyframe && !key)
    {
        r->stats.frames_skipped_waiting_keyframe++;
        return;
    }
    r->waiting_keyframe = 0;
    if (r->assembly_cap < f->frame_size)
    {
        free(r->assembly);
        r->assembly = (uint8_t *)malloc(f->frame_size);
        r->assembly_cap = r->assembly ? f->frame_size : 0;
        if (!r->assembly)
            return;
    }
    // Every non-last block carries the same k, so block b starts at b * k0 * S.
    const size_t blockBytes = (size_t)(f->block_count > 1 ? f->full_k : f->blocks[0].k) * f->blocks[0].shard_size;
    uint32_t off = 0;
    for (unsigned b = 0; b < f->block_count; b++)
    {
        const UdpmRxBlock *blk = &f->blocks[b];
        const uint32_t want =
            (uint32_t)(f->frame_size - b * blockBytes < blockBytes ? f->frame_size - b * blockBytes : blockBytes);
        memcpy(r->assembly + off, blk->buf, want);
        off += want;
    }
    r->stats.frames_delivered++;
    if (r->on_frame)
        r->on_frame(r->ctx, r->type, f->frame_id, r->assembly, f->frame_size, f->timestamp_us, f->flags);
}

void UdpmReceiverPoll(UdpmReceiver *r, uint64_t now_us)
{
    if (!r->started)
        return;
    for (;;)
    {
        UdpmRxFrame *f = slotFor(r, r->next_id);
        const int present = f->used && f->frame_id == r->next_id;
        if (present && f->blocks_done == f->block_count)
        {
            deliver(r, f);
            freeFrame(f);
            r->next_id++;
            continue;
        }
        // Is any newer frame complete and has it waited long enough for this one?
        int expired = 0;
        for (uint32_t i = 1; i < UDPM_RX_WINDOW; i++)
        {
            const UdpmRxFrame *n = slotFor(r, r->next_id + i);
            if (n->used && n->frame_id == r->next_id + i && n->blocks_done == n->block_count)
            {
                expired = now_us - n->complete_us >= r->hold_us;
                break;
            }
        }
        if (!expired)
            return;
        if (present)
            freeFrame(f);
        r->stats.frames_dropped++;
        if (r->type == UdpmType_Video)
            r->waiting_keyframe = 1;
        r->next_id++;
    }
}

int UdpmReceiverCollectNacks(UdpmReceiver *r, uint64_t now_us, UdpmNack *out, int max)
{
    int n = 0;
    for (unsigned i = 0; i < UDPM_RX_WINDOW && n < max; i++)
    {
        UdpmRxFrame *f = &r->frames[i];
        if (!f->used)
            continue;
        for (unsigned bi = 0; bi < f->block_count && n < max; bi++)
        {
            UdpmRxBlock *b = &f->blocks[bi];
            // Only once the block has gone quiet: a paced block legitimately takes ~20 ms to arrive.
            if (!b->buf || b->done || b->nacks >= 2 ||
                now_us - b->last_us < (uint64_t)r->nack_after_us * (b->nacks + 1))
                continue;
            UdpmNack *x = &out[n];
            memset(x, 0, sizeof(*x));
            x->frame_id = f->frame_id;
            x->block_index = (uint8_t)bi;
            x->stream_type = r->type;
            for (unsigned s = 0; s < (unsigned)b->k + b->m; s++)
            {
                if (b->present[s])
                    continue;
                if (s < 64)
                    x->missing_lo |= 1ULL << s;
                else
                    x->missing_hi |= 1ULL << (s - 64);
            }
            b->nacks++;
            n++;
        }
    }
    return n;
}

