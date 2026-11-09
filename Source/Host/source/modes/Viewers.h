#pragma once
#include <switch.h>
#include <stdbool.h>
#include "../capture.h"

// Each network viewer has its own video queue, audio cursor, socket and controller slot.
// The encoder submits video directly to the viewer queues.
//
// The release accepts up to VIEWER_MAX simultaneous viewers.
// USB bridge hosting is exclusive with network viewer sessions.
#define VIEWER_MAX 3

typedef enum
{
    ViewerKind_Punch,
    ViewerKind_Lan
} ViewerKind;

typedef struct Viewer Viewer;

void Viewers_Init(void); // installs the socket send hook; safe to call more than once
int Viewers_Max(void);

// ---- session places -------------------------------------------------------------------------------
bool Viewers_UsbTryAcquire(void);
// USB wins over network viewers: asks every reserved viewer to end its session. A kicked client may
// reconnect by itself or manually once USB is released; while USB is attached new viewers are refused.
void Viewers_KickAll(void);
void Viewers_UsbRelease(void);
// Reserves a place for a session that is about to start. NULL if USB is attached, every place is taken, or (Punch)
// another punch attempt is still registering/waiting for its connector (the rendezvous server keeps one waiting host
// per id).
Viewer *Viewers_Reserve(ViewerKind kind);
void Viewers_Release(Viewer *v);        // end of the session; also detaches it if it was streaming
void Viewers_PunchPhaseDone(Viewer *v); // the punch attempt got its peer (or gave up): another attempt may now start
bool Viewers_PunchWaiting(void);        // some punch attempt is still waiting for a connector
int Viewers_Count(void);                // places in use (waiting + streaming)
int Viewers_Slot(const Viewer *v);

// Thread storage for the viewer's own session thread (the caller launches it with LaunchThread).
Thread *Viewers_Thread(Viewer *v);
void *Viewers_StackBase(Viewer *v);
u32 Viewers_StackSize(void);
bool Viewers_ThreadStarted(const Viewer *v);
void Viewers_MarkThreadStarted(Viewer *v);

// ---- streaming ------------------------------------------------------------------------------------
// Call once the handshake succeeds; batching and audio codec follow the request.
void Viewers_Attach(Viewer *v, int socket, bool wantAudio, int audioBatching, bool opusAudio);
void Viewers_Detach(Viewer *v);
// Next queued frame as wire bytes (header + payload); waits up to waitMs when none is queued.
bool Viewers_NextVideo(Viewer *v, const u8 **wire, u32 *wireSize, u32 waitMs);
// Next audio batch for this viewer, if one is ready (never blocks).
bool Viewers_NextAudio(Viewer *v, const u8 **wire, u32 *wireSize);
// True when nothing decodable arrived for 20 s while viewers were attached (the old "no video frame for 20s" rule).
bool Viewers_StreamStale(void);
// Raw capture can advance when the fastest active viewer has no queued frame.
bool Viewers_VideoCanProduce(void);
// complete AUs go directly to bounded per-viewer mailboxes. A ticket is
// complete only after one participating sender has written the full wire packet.
bool Viewers_VideoActive(void);
bool Viewers_VideoSubmit(const void *data, u32 size, u64 timestamp);
bool Viewers_VideoWaitDelivery(void);
u32 Viewers_VideoBudgetBps(void);
void Viewers_EnableFeedback(Viewer *v, bool enabled, bool udp);
// Existing session thread becomes media-only; this worker exclusively owns input.
bool Viewers_VideoStartInput(Viewer *v);
bool Viewers_VideoInputAlive(Viewer *v);
bool Viewers_VideoSend(Viewer *v, const void *wire, u32 size, bool video);
// NXRP: same completion/feedback accounting as NxrpSend, video sent over UDPM (UdpmVideo.c).
bool Viewers_VideoSendUdp(Viewer *v, const void *wire, u32 size);
// Reads this viewer's pending controller/touch bytes and services held touch/HOME. False once the peer is gone.
bool Viewers_ServiceInput(Viewer *v);
