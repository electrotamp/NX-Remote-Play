// UDPM receiver core (reference implementation; the Android client ports it to Kotlin).
// Reassembles blocks, rebuilds lost shards with FEC, delivers frames in order and
// never hands the decoder a frame whose reference chain was broken.
#pragma once
#include "udpm_proto.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define UDPM_RX_WINDOW 32 // frames in flight per stream

    typedef struct
    {
        uint8_t *buf; // (k+m) * shard_size
        uint8_t present[UDPM_MAX_DATA_SHARDS + UDPM_MAX_PARITY_SHARDS];
        uint8_t k, m, have, done, nacks;
        uint16_t shard_size;
        uint64_t last_us; // most recent shard arrival
    } UdpmRxBlock;

    typedef struct
    {
        int used;
        uint32_t frame_id, frame_size;
        uint8_t block_count, blocks_done, flags;
        uint8_t full_k; // data shards of every block but the last (0 until one of them arrives)
        uint64_t timestamp_us, complete_us;
        UdpmRxBlock blocks[255];
    } UdpmRxFrame;

    typedef void (*UdpmFrameFn)(void *ctx, uint8_t type, uint32_t frame_id, const uint8_t *data, uint32_t size,
                                uint64_t timestamp_us, uint8_t flags);

    typedef struct
    {
        uint32_t packets_received, shards_recovered_fec;
        uint32_t frames_delivered, frames_dropped, frames_skipped_waiting_keyframe;
        uint32_t highest_seq, first_seq;
        int seq_seen;
        uint32_t jitter_us; // RFC 3550 style interarrival jitter on frame timestamps
    } UdpmRxStats;

    typedef struct
    {
        uint8_t type;
        uint32_t session;
        uint32_t next_id;
        int started;
        int waiting_keyframe; // set after a drop; cleared by the next complete keyframe
        uint32_t hold_us;     // how long a complete newer frame waits for an older incomplete one
        uint32_t nack_after_us;
        UdpmRxFrame *frames; // UDPM_RX_WINDOW slots (heap)
        uint8_t *assembly;   // scratch for in-order delivery
        uint32_t assembly_cap;
        UdpmFrameFn on_frame;
        void *ctx;
        int64_t transit_prev;
        int transit_init;
        UdpmRxStats stats;
    } UdpmReceiver;

    int UdpmReceiverInit(UdpmReceiver *r, uint8_t type, uint32_t session, UdpmFrameFn on_frame, void *ctx);
    void UdpmReceiverFree(UdpmReceiver *r);

    // Feed one datagram. now_us is the local monotonic clock.
    // Returns 1 if accepted, 0 if ignored (wrong stream/session, stale, malformed).
    int UdpmReceiverPush(UdpmReceiver *r, const uint8_t *dg, size_t len, uint64_t now_us);

    // Delivers/drops frames whose wait has expired. Call at least every few ms.
    void UdpmReceiverPoll(UdpmReceiver *r, uint64_t now_us);

    // Fills up to `max` NACKs for blocks that are still short of k shards after
    // nack_after_us. Each block is NACKed at most twice.
    int UdpmReceiverCollectNacks(UdpmReceiver *r, uint64_t now_us, UdpmNack *out, int max);

    // 1 once a keyframe request should be sent (the caller rate-limits it).
    static inline int UdpmReceiverNeedsKeyframe(const UdpmReceiver *r)
    {
        return r->waiting_keyframe;
    }

#ifdef __cplusplus
}
#endif
