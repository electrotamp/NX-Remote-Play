#pragma once
#include <switch.h>

#define STREAM_PACKET_HEADER 0xCCCCCCCC

/*
    This is higher than what suggested on switchbrw to fix a known capture issue,
    See the comment in ReadVideoStream()
*/
#define VbufSz 0x54000

/*
    Audio is 16bit pcm at 48000hz stereo. In official software it's read in 0x1000 chunks
    that's 1024 samples per chunk (2 bytes per sample and stereo so divided by 4)
    (1 / 48000) * 1024 is 0,02133333 seconds per chunk.
    Smaller buffer sizes don't seem to work, only tested 0x400 and grc fails with 2212-0006
*/
#define AbufSz 0x1000

/*
    Audio batching adds some delay to the audio streaming in exchange for less pressure on
    the USB and network protocols. A batching of 1 halves the number of audio transfers while
    adding about a frame of delay.
    This is acceptable as grc:d already doesn't provide real time audio.
    To disable support set the following to 0
*/
#define MaxABatching 5

#define DefaultABatching 3

enum PacketMeta
{
    PacketMeta_Type_Video = BIT(0),
    PacketMeta_Type_Audio = BIT(1),
    PacketMeta_Content_Data = BIT(2),
    PacketMeta_Content_MultiNal = BIT(4), // Only if PacketMeta_Type_Video

    // Audio packets only: the payload is one Opus packet (48 kHz stereo, 20 ms) and not raw PCM. Sent only to
    // clients that set ExtraFeatureFlags_OpusAudio in the request.
    PacketMeta_Audio_Opus = BIT(7)
};

typedef struct
{
    u32 Magic;
    u32 DataSize;
    u64 Timestamp; // timestamps are in usecs
    u8 MetaData;
    // Used by packet Replaying to indicate which slot identifies this packet
    // When MetaData & Data the client should cache this packet with this ID
    // When MetaData & Replay the client should replay the cached packet with this ID
    // 0xFF indicates no hash, for example when streaming with audio or with hashes disabled
    u8 ReplaySlot;
} __attribute__((packed)) PacketHeader;

_Static_assert(sizeof(PacketHeader) == 18);

typedef struct
{
    PacketHeader Header;
    u8 Data[VbufSz];
} VideoPacket;

_Static_assert(sizeof(VideoPacket) == sizeof(PacketHeader) + VbufSz);

typedef struct
{
    PacketHeader Header;
    u8 Data[AbufSz * (1 + MaxABatching)];
} AudioPacket;

_Static_assert(sizeof(AudioPacket) == sizeof(PacketHeader) + AbufSz * (1 + MaxABatching));

extern VideoPacket VPkt;
extern AudioPacket APkt;

Result CaptureInitialize();

// Captures video with grc:d, if no game is running this blocks and there's no way to terminate the call
bool CaptureReadVideo();

// True if a real (non-screenshot-fallback) grc:d video frame has arrived within the last
// windowNs nanoseconds - i.e. a game is very likely currently focused and being recorded.
// Reads the always-on video pump thread's live state directly, so unlike
// CaptureReadVideo()'s own per-client bookkeeping this works even with no client connected.
bool Capture_IsRealVideoRecentlyActive(u64 windowNs);

// Captures audio with grc:d, if no game is running this blocks and there's no way to terminate the call

// Non-blocking: true (APkt filled) only if a full AudioBatching+1 batch is already buffered.
// Call CaptureAudioConnected() first. Used by PunchProbe.c to interleave audio with video.
bool CaptureTryReadAudio();

//  (multi-viewer): non-blocking audio read with a caller-owned cursor and packet, for several viewers at
// once. CaptureAudioCursorNow() is the cursor for "start from whatever is produced next".
u64 CaptureAudioCursorNow(void);
bool CaptureTryReadAudioFor(u64 *cursor, int batching, AudioPacket *out);

// When a client first connects clear old data structures and prepare hashing modes
void CaptureVideoConnected();
void CaptureAudioConnected();

// True while the screenshot source is patching VI; input waits for it.
bool Capture_IsViPatchBusy(void);

// Configurable options
int CaptureSetAudioBatching(int batch);

// Passive audio producer progress, logged by the independent hub.
