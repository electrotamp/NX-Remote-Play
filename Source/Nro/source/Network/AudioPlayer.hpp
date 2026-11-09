#pragma once
#include <string>
#include <cstdint>
#include <cstddef>

// Game audio for the NRO . The console / desktop bridge interleave PCM packets
// (PacketMeta_Type_Audio, signed 16-bit little-endian, 48 kHz stereo, 0x1000 bytes per batch unit) on the
// same TCP stream as the video when the handshake asks for them (MetaFlags bit 1). PunchConnector's
// receive thread hands each payload to Push(); a dedicated thread feeds libnx audout from a small
// bounded queue, so audio can never stall video or the UI, and a backlog is dropped (oldest first)
// rather than turned into delay.
//
namespace NroAudio
{
// Opens audout and starts the playback thread. False (and audio stays off) if audout fails;
// never fatal for the video session. `log` receives diagnostic lines (nro_log.txt).
bool Start(void (*log)(const std::string &));
// Queues one PCM payload (any thread). Ignored when not started.
void Push(const uint8_t *pcm, size_t size);
// Stops playback and closes audout. Safe if never started.
void Stop();
} // namespace NroAudio
