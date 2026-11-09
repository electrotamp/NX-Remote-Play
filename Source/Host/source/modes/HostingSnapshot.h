#pragma once
// Shared between the sysmodule (modes/SessionHosting.h/.c, which owns this
// data) and the NRO (NxRemotePlayConfig/source/ipc.h/.c, which reads it via
// CMD_GET_HOSTING_STATE) - deliberately its own tiny header, no <switch.h>
// dependency, so the NRO's non-Switch preview builds (native Windows,
// WebAssembly) can include it too without pulling in Switch-only types.

typedef enum {
	HostingState_Idle,
	HostingState_Requesting,
	HostingState_Registered,
	HostingState_Error,
	// The user has turned hosting off via the NRO's Settings toggle (see
	// SessionHosting_SetEnabled) - no REGISTER/REFRESH traffic to the
	// rendezvous server happens at all while in this state, distinct from
	// Idle (which only means "hasn't registered yet").
	HostingState_Disabled,
} HostingState;

// id/password are 5-char + NUL; errorMessage is truncated to fit, always
// NUL-terminated.
typedef struct {
	HostingState state;
	char id[6];
	char password[6];
	char errorMessage[48];
	// One-line notice for the Host screen's status line (empty = none), e.g. "Your Session ID was already
	// taken on the server, so a new one was issued automatically." noticeAgeSec is how long ago it was
	// raised so the screen can hide it again after a while .
	unsigned int noticeAgeSec;
	char notice[96];
} HostingSnapshot;
