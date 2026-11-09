#pragma once
#include <switch.h>

// Manually-triggered, single-attempt TCP hole-punch MECHANICS probe. This is deliberately
// scoped to exactly one new variable, on top of what's already hardware-proven:
//
//   - RelayProbe.c's exact shape (a bounded, manually-triggered, single outbound TCP
//     connect, held up to 60s, then closed, no retry) is already hardware-confirmed safe
//     across sleep AND Restart . That question is answered - this probe does
//     not need to re-prove it.
//   - The one genuinely untested thing for real TCP hole-punching: connect()ing out from
//     an EXPLICITLY CHOSEN local port (SO_REUSEADDR) instead of an OS-assigned ephemeral
//     one - required because simultaneous-open needs both sides connecting from the same
//     local port their peer was told about. This probe tests only whether bind()-then-
//     connect() to a specific local port works AT ALL on this platform/libnx version - not
//     yet a real punch against a live peer, and NOT bound to TCP_MODE's real 9911/9922
//     ports (uses its own dedicated test port instead), so a failure here can never affect
//     real video/audio streaming.
//
// Loads the rendezvous server (server.txt or the built-in default) for PunchWake.c and starts the button watcher.
void PunchProbe_StartButtonWatcher(FsFileSystem *sdCard);

// Fires a probe attempt from a non-button trigger (PunchWake.c's UDP CHECKIN loop). See
// PunchProbe.c's definition for why this skips the button watcher's game-focus guard.
void PunchProbe_RequestWake(void);

// USB holds the same exclusive session gate as LAN/punch. This is capture
// ownership only; authenticated rendezvous delegation is a later stage.
bool PunchProbe_TryAcquireUsb(void);
// USB wins over network viewers: ends every network session, including a punch attempt still waiting for its connector.
void PunchProbe_EvictViewersForUsb(void);
void PunchProbe_ReleaseUsb(void);

// Called by PunchWake.c once the server has issued (or resumed) this console's Session
// ID/Password. PunchProbe uses it for PUNCHHOST.
void PunchProbe_SetIdentity(const char *id, const char *password);

// Repoints PunchHOST at a different Rendezvous server (called by PunchWake.c when the NRO
// changes the server); takes effect on the next punch attempt.
void PunchProbe_SetRelay(const char *host, u16 port);

// Shared server file: /config/nxremoteplay/server.txt, written ONLY by the NRO and parsed by
// net/server_config.h (identical copy lives in the NRO). Returns true and the effective "host:port" when the file
// exists and configures a server. Used at boot (overrides the built-in default) and by PunchWake every cycle.
bool PunchProbe_ReadServerFile(FsFileSystem *sd, char *effOut, size_t effSize);

// Exposes the relay/id/password already loaded by PunchProbe so PunchWake.c doesn't
// need its own copy of that config. False if no config has been successfully loaded yet.
bool PunchProbe_GetRelayConfig(char *outHost, size_t hostSize, u16 *outPort, char *outId, size_t idSize,
                               char *outPassword, size_t passwordSize);
