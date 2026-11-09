// UDPM sender: splits an encoded frame into FEC blocks, paces the datagrams with a
// token bucket and keeps recent datagrams for NACK retransmission.
// Single-threaded by design: call everything from the one media writer thread.
#pragma once
#include "udpm_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns 0 when the datagram was handed to the network, <0 on a fatal error.
// The sysmodule glue maps EAGAIN to a short wait + retry and counts it.
typedef int (*UdpmSendFn)(void* ctx, const uint8_t* datagram, size_t len);
typedef uint64_t (*UdpmNowUsFn)(void* ctx);
typedef void (*UdpmSleepUsFn)(void* ctx, uint64_t us);

typedef struct {
	uint16_t shard_size;       // payload bytes per shard (<= UDPM_MAX_SHARD_SIZE)
	uint8_t max_data_shards;   // k per block (<= UDPM_MAX_DATA_SHARDS)
	uint8_t fec_percent;       // parity = ceil(k * fec_percent / 100), clamped below
	uint8_t min_parity, max_parity;
	uint32_t pace_bps;         // pacing rate; set above the encoder bitrate
	uint32_t burst_bytes;      // bucket depth: bytes that may leave back to back
} UdpmSenderConfig;

void UdpmSenderDefaultConfig(UdpmSenderConfig* c);

typedef struct {
	uint64_t tokens_x1e6; // bytes * 1e6 (avoids float)
	uint64_t last_us;
	uint32_t rate_bps, burst_bytes;
	int primed;
} UdpmPacer;

void UdpmPacerInit(UdpmPacer* p, uint32_t rate_bps, uint32_t burst_bytes);
// 0 when `bytes` may be sent now (tokens consumed), otherwise microseconds to wait.
uint64_t UdpmPacerDelayUs(UdpmPacer* p, uint64_t now_us, uint32_t bytes);

typedef struct {
	uint8_t type;
	uint32_t frame_id;
	uint8_t block_index, shard_index;
	uint16_t len;
	uint8_t valid;
	uint8_t data[UDPM_MAX_DATAGRAM];
} UdpmRtxSlot;

typedef struct {
	uint32_t frames, retransmits;
	uint64_t bytes, paced_wait_us;
} UdpmSenderStats;

typedef struct {
	UdpmSenderConfig cfg;
	uint32_t session;
	uint32_t next_frame_id[4]; // indexed by UdpmType (1..3)
	uint32_t packet_seq[4];
	UdpmPacer pacer;
	UdpmRtxSlot* rtx;          // caller-owned ring, may be NULL (no retransmission)
	uint32_t rtx_count, rtx_next;
	UdpmSendFn send; UdpmNowUsFn now; UdpmSleepUsFn sleep; void* io;
	UdpmSenderStats stats;
	uint8_t parity[UDPM_MAX_PARITY_SHARDS][UDPM_MAX_SHARD_SIZE];
	uint8_t pad[UDPM_MAX_SHARD_SIZE];
	uint8_t datagram[UDPM_MAX_DATAGRAM];
} UdpmSender;

void UdpmSenderInit(UdpmSender* s, const UdpmSenderConfig* cfg, uint32_t session,
	UdpmRtxSlot* rtx, uint32_t rtx_count,
	UdpmSendFn send, UdpmNowUsFn now, UdpmSleepUsFn sleep, void* io);

// Parity shard count the sender will use for a block of k data shards.
unsigned UdpmSenderParityFor(const UdpmSenderConfig* c, unsigned k);

// Sends one access unit (video/audio/probe). Blocks (paced) until every datagram is out.
// Returns the frame id used, or -1 on a send error (remaining datagrams are skipped).
int64_t UdpmSenderSendFrame(UdpmSender* s, uint8_t type, const uint8_t* frame, uint32_t size,
	uint64_t timestamp_us, uint8_t flags);

// Resends the shards named by a NACK that are still cached. Returns how many were resent.
int UdpmSenderHandleNack(UdpmSender* s, const UdpmNack* n);

// Runtime adjustments from receiver feedback (rate control, adaptive FEC).
void UdpmSenderSetPace(UdpmSender* s, uint32_t pace_bps);

#ifdef __cplusplus
}
#endif
