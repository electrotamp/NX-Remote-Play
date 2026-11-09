# UDPM - UDP media protocol (version 1)

Media (video, later audio) travels console -> client over UDP. Setup, input and authentication stay on the existing TCP channel, which hands out the 32-bit `session` token that every UDPM datagram carries. Code: `Source/Host/source/udpm/` (C, shared by the sysmodule and PC tools) and `Source/Android/app/src/main/java/com/nxremoteplay/punchclient/udpm/` (Kotlin port). All fields are little-endian.

## Why each piece exists

| Piece | Problem it fixes | Evidence |
|---|---|---|
| UDP instead of TCP for media | One lost packet stalls every later byte on TCP (head-of-line blocking plus retransmission timeouts) | Measured punched path had 27 frames with >50 ms ready-video occupancy versus 0 on LAN; worst 223 ms versus 21 ms |
| 1200-byte shards | IP fragmentation; 1200 + 36 + 28 stays under the 1280-byte IPv6 minimum MTU on any path | Nintendo GameShare uses 1468 B on local wireless; we target internet paths |
| Pacing (token bucket, 2x encoder rate, 16 KiB burst) | A frame sent as one burst overflows the console's UDP buffer and the Wi-Fi queue | Earlier WebRTC build logged `rtp agent_send failed` (dropped on the console itself) |
| FEC per block (Cauchy Reed-Solomon, GF(2^8)/0x11D) | Losses are rebuilt with zero added round trips | Same field and approach as GameShare (`nerd::moecc`) |
| NACK + retransmit cache | Losses beyond FEC still get a second chance while the frame is useful | libpeer treated NACK as "send a whole keyframe" |
| Keyframe gate + keyframe request | The decoder never sees a frame whose references were lost (no smearing) | WebRTC build: "occasional jumbled frames" |
| Playout clock (Nintendo high-water-mark rule) | Bursty arrival becomes even presentation | GameShare `HighWaterMarkLatencyEstimator` / `FastVideoSchedPolicy` |

## Datagrams

### Media shard (type 1 video, 2 audio, 3 probe) - 36-byte header + `shard_size` payload

| Off | Size | Field |
|---|---|---|
| 0 | 1 | magic `0xD7` |
| 1 | 1 | version `1` |
| 2 | 1 | type |
| 3 | 1 | flags: bit0 keyframe, bit1 parity shard, bit2 retransmission |
| 4 | 4 | session |
| 8 | 4 | packet_seq (per stream, every datagram incl. parity and retransmits; loss statistics) |
| 12 | 4 | frame_id (per stream, +1 per frame) |
| 16 | 4 | frame_size (bytes before padding) |
| 20 | 1 | block_index |
| 21 | 1 | block_count |
| 22 | 1 | shard_index (0..k-1 data, k..k+m-1 parity) |
| 23 | 1 | data_shards k (1..64) |
| 24 | 1 | parity_shards m (0..32) |
| 25 | 1 | reserved 0 |
| 26 | 2 | shard_size (1..1400; every shard in a block has this size, the frame's last data shard is zero-padded) |
| 28 | 8 | timestamp_us (host capture clock) |

A frame is split into blocks of at most `k = 32` data shards (38,400 bytes); block `b` carries bytes `[b*k0*S, ...)` where `k0` is block 0's k. Parity per block: `m = clamp(ceil(k * fec_percent / 100), min_parity=2, max_parity=12)`, default `fec_percent = 20`.

Parity shard `p`, byte `i`: `sum over d of C[p][d] * data[d][i]` in GF(2^8) with polynomial `0x11D`, `C[p][d] = 1 / (p XOR (m + d))`. Any k of the k+m shards rebuild the block.

### Client -> host (8-byte prefix: magic, version, type, flags=0, session)

| Type | Size | Body after the prefix |
|---|---|---|
| `0x10` Hello | 12 | u32 nonce. Opens the NAT path and keeps the stream alive; send every 1 s. Host stops after 5 s of silence. |
| `0x11` Nack | 32 | u32 frame_id, u8 block_index, u8 stream_type, u16 0, u64 missing_lo, u64 missing_hi (bit n = shard n missing) |
| `0x12` Feedback | 40 | u32 seq, highest_frame_id, packets_received, packets_lost, shards_recovered_fec, frames_dropped, jitter_us, playout_delay_us |
| `0x13` KeyframeRequest | 12 | u32 last_good_frame_id. Rate-limit to 5/s. |

## Receiver rules (identical in C and Kotlin)

1. Frames are delivered strictly in `frame_id` order.
2. A block completes as soon as any k shards are present; missing data shards are rebuilt by FEC.
3. If a newer frame is complete and the oldest pending frame is still incomplete after `hold_us` (40 ms), the old frame is dropped.
4. After a video drop, P-frames are withheld until the next keyframe arrives complete, and the client sends KeyframeRequest.
5. A block still short of k shards after `nack_after_us` (8 ms), and again after 16 ms, is NACKed with the exact missing shard bitmap.

## Not yet decided / owed
- Audio over UDPM (PCM at 1.536 Mbps fits; Opus would cut it to ~0.1 Mbps).
- Adaptive FEC and bitrate from Feedback (fields are there; policy is not).
- Encryption/authentication beyond the session token (the TCP channel is authenticated; UDP datagrams are not yet MAC'd).
- NAT traversal for the UDP media port (the rendezvous already speaks UDP on 7921).
