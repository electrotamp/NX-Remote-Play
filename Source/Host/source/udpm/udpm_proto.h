// UDP media protocol (UDPM) wire format. Portable C, no libnx dependency, so the
// same code builds into the sysmodule and the PC/host test tools.
// Every multi-byte field is little-endian. See NXRP/PROTOCOL.md.
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UDPM_MAGIC 0xD7
#define UDPM_VERSION 1
#define UDPM_HEADER_SIZE 36

// Payload bytes per shard. 1200 + 36 + 28 (IPv4/UDP) stays under the 1280-byte
// IPv6 minimum MTU, so no path (Wi-Fi, LTE, VPN) fragments a datagram.
#define UDPM_DEFAULT_SHARD_SIZE 1200
#define UDPM_MAX_SHARD_SIZE 1400
#define UDPM_MAX_DATAGRAM (UDPM_HEADER_SIZE + UDPM_MAX_SHARD_SIZE)

// Per-block shard limits. Blocks stay small so FEC decode latency and cost stay
// bounded; large frames simply use more blocks.
#define UDPM_MAX_DATA_SHARDS 64
#define UDPM_MAX_PARITY_SHARDS 32

typedef enum {
	UdpmType_Video = 1,       // host -> client, one data or parity shard
	UdpmType_Audio = 2,       // host -> client, one data or parity shard
	UdpmType_Probe = 3,       // host -> client, synthetic shard (transport probe)
	UdpmType_Hello = 0x10,    // client -> host, opens/refreshes the path (also NAT keepalive)
	UdpmType_Nack = 0x11,     // client -> host, missing shards of one block
	UdpmType_Feedback = 0x12, // client -> host, periodic receive statistics
	UdpmType_KeyframeRequest = 0x13, // client -> host, decoder lost its reference chain
	UdpmType_Congestion = 0x14, // client -> host, delay and starvation seen in the last interval
} UdpmType;

enum {
	UdpmFlag_Keyframe = 1u << 0, // the frame is an IDR access unit
	UdpmFlag_Parity = 1u << 1,   // this shard is a parity shard
	UdpmFlag_Retransmit = 1u << 2,
};

// Media shard header (types Video/Audio/Probe), 36 bytes:
//  0 u8  magic            1 u8  version        2 u8 type     3 u8 flags
//  4 u32 session          (token handed out on the TCP control channel)
//  8 u32 packet_seq       (per stream, every datagram incl. parity/retransmits)
// 12 u32 frame_id         (per stream, increments by one per frame)
// 16 u32 frame_size       (bytes of the access unit before padding)
// 20 u8  block_index      21 u8 block_count
// 22 u8  shard_index      (0..k-1 data, k..k+m-1 parity)
// 23 u8  data_shards (k)  24 u8 parity_shards (m)   25 u8 reserved
// 26 u16 shard_size       (payload bytes of every shard in this block)
// 28 u64 timestamp_us     (host capture time, host clock)
typedef struct {
	uint8_t type, flags;
	uint32_t session, packet_seq, frame_id, frame_size;
	uint8_t block_index, block_count, shard_index, data_shards, parity_shards;
	uint16_t shard_size;
	uint64_t timestamp_us;
} UdpmShardHeader;

// Control messages share the first 8 bytes: magic, version, type, flags, session.
// Nack body (after 8 bytes): u32 frame_id, u8 block_index, u8 stream_type, u16 reserved,
//                            u64 missing_mask_lo, u64 missing_mask_hi   (bit n = shard n) -> 32 bytes total
// Feedback body: u32 seq, u32 highest_frame_id, u32 packets_received, u32 packets_lost,
//                u32 shards_recovered_fec, u32 frames_dropped, u32 jitter_us, u32 playout_delay_us
//                -> 40 bytes total
// Congestion body: u32 seq, u32 interval_ms, u32 frames, u32 excess_min_us, u32 excess_max_us, u32 nacks,
//                  u32 audio_underruns, u32 reserved (0) -> 40 bytes total. Hosts that do not know the type ignore it.
// KeyframeRequest body: u32 last_good_frame_id -> 12 bytes total
// Hello body: u32 client_nonce -> 12 bytes total
#define UDPM_NACK_SIZE 32
#define UDPM_FEEDBACK_SIZE 40
#define UDPM_CONGESTION_SIZE 40
#define UDPM_KEYFRAME_REQUEST_SIZE 12
#define UDPM_HELLO_SIZE 12

typedef struct {
	uint32_t frame_id;
	uint8_t block_index, stream_type;
	uint64_t missing_lo, missing_hi;
} UdpmNack;

typedef struct {
	uint32_t seq, highest_frame_id, packets_received, packets_lost;
	uint32_t shards_recovered_fec, frames_dropped, jitter_us, playout_delay_us;
} UdpmFeedback;

typedef struct {
	uint32_t seq, interval_ms, frames, excess_min_us;
	uint32_t excess_max_us, nacks, audio_underruns, reserved;
} UdpmCongestion;

size_t UdpmWriteShardHeader(uint8_t* out, const UdpmShardHeader* h);
// Returns 0 when the datagram is not a well-formed v1 media shard.
int UdpmReadShardHeader(const uint8_t* in, size_t len, UdpmShardHeader* h);

// Control messages. Read functions return 0 on malformed input or session mismatch.
size_t UdpmWriteNack(uint8_t* out, uint32_t session, const UdpmNack* n);
int UdpmReadNack(const uint8_t* in, size_t len, uint32_t session, UdpmNack* n);
size_t UdpmWriteFeedback(uint8_t* out, uint32_t session, const UdpmFeedback* f);
int UdpmReadFeedback(const uint8_t* in, size_t len, uint32_t session, UdpmFeedback* f);
size_t UdpmWriteCongestion(uint8_t* out, uint32_t session, const UdpmCongestion* c);
int UdpmReadCongestion(const uint8_t* in, size_t len, uint32_t session, UdpmCongestion* c);
size_t UdpmWriteKeyframeRequest(uint8_t* out, uint32_t session, uint32_t last_good_frame_id);
int UdpmReadKeyframeRequest(const uint8_t* in, size_t len, uint32_t session, uint32_t* last_good_frame_id);
size_t UdpmWriteHello(uint8_t* out, uint32_t session, uint32_t nonce);
int UdpmReadHello(const uint8_t* in, size_t len, uint32_t session, uint32_t* nonce);
// Type byte of any UDPM datagram, or 0 if it is not one.
int UdpmPeekType(const uint8_t* in, size_t len);

#ifdef __cplusplus
}
#endif
