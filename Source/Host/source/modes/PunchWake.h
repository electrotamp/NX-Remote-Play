#pragma once
#include <switch.h>
#include "HostingSnapshot.h"
bool PunchWake_IsTemporarilySuspended(void);
bool PunchWake_TemporarilySuspend(u64 deadline);
void PunchWake_ClearTemporarySuspension(void);
bool PunchWake_GetDirectTicket(char *ticket, size_t size);
bool PunchWake_DirectOwnershipEnabled(void);
// Switches ordinary hosting to authenticated (owner-key) mode live, right after a first-use key commit.
void PunchWake_SetDirectOwnership(bool enabled);

// Rendezvous Session ID/Password client. Register or resume the persisted identity,
// refresh it periodically and request a wake when REFRESH returns WAKE. Use
// short-lived UDP exchanges so signaling does not hold a socket across sleep.
// Start only when server configuration and the hosting opt-in flag are present.
void PunchWake_Start(FsFileSystem *sdCard);

// Current Session ID/Password once registered. False until the first successful REGISTER.
// Persisted/current local identity, also available without rendezvous registration or hosting on.
bool PunchWake_GetLocalIdentity(char *outId, size_t idSize, char *outPassword, size_t passwordSize);

// Explicit user request for a brand-new id/password (the only non-age-out way one changes).
void PunchWake_RequestNewCode(void);

// NRO IPC (CMD_GET_HOSTING_STATE / CMD_SET_HOSTING_ENABLED): live state + on/off toggle
// (persisted in /config/nxremoteplay/hosting_enabled.txt; off = no REGISTER/REFRESH, no punching).
// Rendezvous server "host:port" (NRO Settings > Server). SetServer validates (non-empty host,
// port 1-65535), applies it without persisting (the NRO writes server.txt) and makes the session thread
// re-register (resuming the same Session ID/Password) at the new server; false = rejected,
// nothing changed.
void PunchWake_GetServer(char *out, size_t outSize);
bool PunchWake_SetServer(const char *hostPort);

void PunchWake_GetSnapshot(HostingSnapshot *out);
void PunchWake_SetEnabled(bool enabled);
