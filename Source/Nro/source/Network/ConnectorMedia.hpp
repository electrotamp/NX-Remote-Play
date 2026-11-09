#pragma once
#include "../UI/VideoTexture.hpp"
#include "../UI/imgui/imgui.h"

// Connects complete Annex-B access units to hardware decoding and the video texture.
namespace ConnectorMedia
{
// Registers this module's callback with
// Network::Rendezvous::Connector::SetOnVideoFrame - must happen before
// Connect() is called (see that function's own doc comment), so this is
// called once from the Connect screen rather than from the playback
// scene itself. Safe to call repeatedly (idempotent).
void EnsureVideoCallbackRegistered();

// Session pairing: the punched NxRemotePlay stream already delivers Annex-B H.264 access
// units (no RTP), so the video callback feeds them straight to the decoder. Call when a
// backlog was dropped (or a new connection starts) so decoding restarts on the next
// self-contained keyframe instead of feeding P-frames with no reference (crashes NVTEGRA).
void ResetVideoSync();

// false = the next video access units are only decoded (references), not shown. Poll
// clears it for the older frames of a backlog and sets it again for the newest one.
void SetPresentFrames(bool present);

// True while video access units are being discarded because decoding has not synced on a keyframe yet
// (a new session, a backlog drop or a decoder error). The caller asks the host for a keyframe meanwhile.
bool WaitingForKeyframe();

// Decoder errors since the last call (each one restarted decoding on the next keyframe).
unsigned TakeDecoderErrors();

// What the sync check saw since the last call: access units discarded while unsynced, how many
// held an SPS, PPS, IDR or other slice, the size of the last one, and how often it synced.
struct SyncStats
{
    unsigned unsyncedAus = 0, withSps = 0, withPps = 0, withIdr = 0, withSlice = 0, syncs = 0;
    size_t lastSize = 0;
};
SyncStats TakeSyncStats();

// Which picture to draw: whichever source (H.264 video or JPEG still) delivered the newest frame
// this session. NewSession() (called when a connection starts) forgets everything from before.
bool ShowVideo();
bool ShowStill();
void NewSession();

// Owns the texture the playback scene draws every frame.
VideoTexture &GetVideoTexture();

// HOME/unfocused still-frame relay (, reintroduced JPEG
// fallback) - a periodically-refreshed JPEG still the Host sends whenever
// it has no live H.264 to encode (its own app isn't foregrounded), so the
// Connector screen shows something to navigate by instead of a blank/
// frozen frame. Independent of VideoTexture/HasFrame() above - the
// playback scene should prefer live video whenever HasFrame() is true and
// only fall back to this when it isn't. Registered the same way as
// EnsureVideoCallbackRegistered() (idempotent, safe to call every frame).
void EnsureStillFrameCallbackRegistered();
void DrawStillFrame(ImVec2 size);
} // namespace ConnectorMedia
