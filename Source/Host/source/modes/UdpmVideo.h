#pragma once
// NX-Remote-Play host : real video over UDPM for one viewer at a time.
//
// Negotiation rides on the existing TCP handshake: a client that negotiated  delivery
// feedback may also set FeatureFlags bit UDPM_FEATURE_FLAG. If a UDP place is free, the
// response's MemoryPools.ApplicationSize carries UDPM_OFFER_BIT | port << 32 | session token.
// The client then sends UDPM Hello datagrams from its UDP socket; once one arrives from the
// TCP peer's IP with the right token, every video wire packet (18-byte header + payload) goes
// out as one UDPM frame instead of over TCP. Audio, input and control stay on TCP.
// No Hello within the bind window -> the session simply keeps using TCP video.
#include <switch.h>
#include <stdbool.h>

#define UDPM_FEATURE_FLAG 0x20
#define UDPM_VIDEO_PORT 9951
#define UDPM_OFFER_BIT (1ULL << 63)

// Reserves the single UDP video place and returns the response field value (0 = not offered).
u64 UdpmVideo_Offer(void);
void UdpmVideo_NatOffer(u8 key[16]);
// Releases an offer/session (safe to call when none is held).
void UdpmVideo_End(void);
// Waits up to waitMs for the client's Hello from the TCP peer's IP. True = video goes over UDP.
bool UdpmVideo_Bind(int tcpSocket, u32 waitMs, const char *relayIp, u16 relayPort);
// Sends one video wire packet (header + payload). Paced; returns false on a fatal socket error.
// *pacedNs receives the deliberate pacing wait (not transport time).
bool UdpmVideo_Send(const u8 *wire, u32 size, u64 *pacedNs);
bool UdpmVideo_SendAudio(const u8 *wire, u32 size); // one audio wire packet as a UDPM audio frame
// Handles NACK / keyframe request / feedback / hello datagrams. Call every few ms.
void UdpmVideo_Service(void);
// False once the client has been silent on UDP for 5 s.
bool UdpmVideo_Alive(void);
// Atomic snapshot for capture and shared-viewer budget calculations.
u32 UdpmVideo_TargetBps(void);

// Local diagnostics only; maxima from the same stream worker, in microseconds.
void UdpmVideo_RecordLoopTime(u64 audioUs, u64 waitUs);
// Timing histograms printed with the 5 s support lines (ignored while no UDP viewer is bound).
void UdpmVideo_NoteDeliveryWait(u64 us);
void UdpmVideo_NoteCapture(u32 skippedPresents);
void UdpmVideo_NoteAudioPcm(const u8 *pcm, u32 size);
