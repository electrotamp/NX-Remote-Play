#pragma once
#include <switch.h>
#include "BridgeOwnerProtocol.h"
void BridgeOwner_Initialize(FsFileSystem *sdCard);
bool BridgeOwner_Ready(const char *id, u8 fingerprint[32]);
void BridgeOwner_ResetAttachment(void);
// Keeps the already-committed local USB attachment suppression alive after media
// progress. It never renews server ownership or writes to USB.
void BridgeOwner_MaintainAttachment(void);
// status: 0 signed, 1 unavailable, 2 invalid/scoping failure.
u32 BridgeOwner_Sign(const u8 *bytes, size_t size, u8 proof[32]);
bool BridgeOwner_IsProvisioned(const char *id);
bool BridgeOwner_VerifyDirectReply(const u8 *bytes, size_t size);
bool BridgeOwner_SignDirect(const u8 *bytes, size_t size, const char *endpoint, u8 action, u8 proof[32]);
u32 BridgeOwner_Delegate(const u8 *payload, size_t size, u64 attachment, u8 acknowledgement[32]);
// First-use key setup (USB operations 8 and 9). Stage keeps the key in RAM only; Commit writes
// /config/nxremoteplay/bridge_owner.key, loads it live and switches hosting to authenticated mode.
// status: 0 ok, 2 Session ID mismatch, 3 already has a key for this ID, 4 SD write failed, 5 nothing staged.
bool BridgeOwner_CanEnroll(void);
u32 BridgeOwner_StageKey(const u8 *payload, size_t size, u8 fingerprint[32]);
u32 BridgeOwner_CommitKey(const u8 *payload, size_t size, u8 fingerprint[32]);
// The identity changed (new code / 60-day age-out): the old key belongs to the old ID, so forget it.
void BridgeOwner_Remove(void);
