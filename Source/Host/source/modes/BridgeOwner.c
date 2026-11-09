#include "BridgeOwner.h"
#include "PunchWake.h"
#include "BridgeDelegation.h"
#include "DirectOwnerProtocol.h"
#include <string.h>
#define OWNER_KEY_PATH "/config/nxremoteplay/bridge_owner.key"
static bool g_ready;
static u8 g_key[32], g_fingerprint[32];
static char g_id[6], g_owner[129], g_endpoint[257];
static u64 g_generation;
static BridgeDelegation g_delegation;
static FsFileSystem *g_sd;
// First-use setup (RAM only): the staged key never touches the SD card until it is committed.
static bool g_staged;
static u8 g_stageKey[32];
static char g_stageId[6];

void BridgeOwner_Initialize(FsFileSystem *sdCard)
{
    g_sd = sdCard;
    FsFile file;
    if (R_FAILED(fsFsOpenFile(sdCard, OWNER_KEY_PATH, FsOpenMode_Read, &file)))
        return;
    u8 record[48];
    u64 read = 0;
    s64 size = 0;
    Result rc = fsFileGetSize(&file, &size);
    if (R_SUCCEEDED(rc) && size == sizeof(record))
        rc = fsFileRead(&file, 0, record, sizeof(record), FsReadOption_None, &read);
    fsFileClose(&file);
    if (R_FAILED(rc) || read != sizeof(record) || OwnerU32(record) != USB_OWNER_KEY_MAGIC ||
        OwnerU16(record + 4) != 1 || OwnerU16(record + 6) != sizeof(record) || record[13] || record[14] || record[15])
        return;
    for (int i = 8; i < 13; i++)
        if (!strchr("23456789ABCDEFGHJKLMNPQRSTUVWXYZ", record[i]) || record[i] == 0)
            return;
    memcpy(g_id, record + 8, 5);
    g_id[5] = 0;
    memcpy(g_key, record + 16, 32);
    memset(record, 0, sizeof(record));
    sha256CalculateHash(g_fingerprint, g_key, sizeof(g_key));
    g_ready = true;
}
bool BridgeOwner_Ready(const char *id, u8 fingerprint[32])
{
    if (!g_ready || strcmp(id, g_id) != 0)
        return false;
    memcpy(fingerprint, g_fingerprint, 32);
    return true;
}
bool BridgeOwner_IsProvisioned(const char *id)
{
    return g_ready && strcmp(id, g_id) == 0;
}
bool BridgeOwner_VerifyDirectReply(const u8 *bytes, size_t size)
{
    const char domain[] = "NXRP-DIRECT-REPLY-1";
    if (!g_ready || size < 64 || size > USB_OWNER_MAX + 64 || memcmp(bytes + 8, g_id, 5))
        return false;
    u8 input[sizeof(domain) - 1 + USB_OWNER_MAX + 32], proof[32];
    memcpy(input, domain, sizeof(domain) - 1);
    memcpy(input + sizeof(domain) - 1, bytes, size - 32);
    hmacSha256CalculateMac(proof, g_key, sizeof(g_key), input, sizeof(domain) - 1 + size - 32);
    u8 difference = 0;
    for (size_t i = 0; i < 32; i++)
        difference |= proof[i] ^ bytes[size - 32 + i];
    memset(proof, 0, sizeof(proof));
    return difference == 0;
}
bool BridgeOwner_SignDirect(const u8 *bytes, size_t size, const char *endpoint, u8 action, u8 proof[32])
{
    UsbOwnerRequest request;
    char owner[12] = "switch:";
    memcpy(owner + 7, g_id, 5);
    if (!g_ready || !OwnerParse(bytes, size, g_id, 0, &request) || request.action != action ||
        request.ownerLength != sizeof(owner) || memcmp(request.owner, owner, sizeof(owner)) ||
        request.endpointLength != strlen(endpoint) || memcmp(request.endpoint, endpoint, strlen(endpoint)))
        return false;
    hmacSha256CalculateMac(proof, g_key, sizeof(g_key), bytes, size);
    return true;
}
void BridgeOwner_ResetAttachment(void)
{
    g_staged = false;
    memset(g_stageKey, 0, sizeof(g_stageKey));
    memset(g_stageId, 0, sizeof(g_stageId));
    memset(g_owner, 0, sizeof(g_owner));
    memset(g_endpoint, 0, sizeof(g_endpoint));
    g_generation = 0;
    memset(&g_delegation, 0, sizeof(g_delegation));
    PunchWake_ClearTemporarySuspension();
}
void BridgeOwner_MaintainAttachment(void)
{
    u64 now = armTicksToNs(svcGetSystemTick()) / 1000000ULL;
    // Refresh near the halfway point rather than for every frame. If streaming
    // stalls, this is not called and the finite local fail-safe expires normally.
    if (!BridgeDelegationActive(&g_delegation, now) || !g_delegation.committed ||
        g_delegation.deadline > now + USB_DELEGATION_MAX_MS / 2)
        return;
    if (!PunchWake_TemporarilySuspend(svcGetSystemTick() + armNsToTicks((u64)USB_DELEGATION_MAX_MS * 1000000ULL)) ||
        !BridgeDelegationMaintain(&g_delegation, now, USB_DELEGATION_MAX_MS))
    {
        memset(&g_delegation, 0, sizeof(g_delegation));
        PunchWake_ClearTemporarySuspension();
    }
}
u32 BridgeOwner_Delegate(const u8 *payload, size_t size, u64 attachment, u8 acknowledgement[32])
{
    memset(acknowledgement, 0, 32);
    if (!g_ready || !g_owner[0] || !PunchWake_DirectOwnershipEnabled())
        return 1;
    if (size != 24 || !OwnerU32(payload) || payload[5] || payload[6] || payload[7] || OwnerU32(payload + 20))
        return 2;
    u64 now = armTicksToNs(svcGetSystemTick()) / 1000000ULL;
    u8 operation = payload[4];
    u64 generation = OwnerU64(payload + 8);
    u32 milliseconds = OwnerU32(payload + 16);
    if (!BridgeDelegationApply(&g_delegation, attachment, g_generation, operation, generation, milliseconds, now))
        return 2;
    if (operation == 2)
        PunchWake_ClearTemporarySuspension();
    else if (!PunchWake_TemporarilySuspend(svcGetSystemTick() + armNsToTicks((u64)milliseconds * 1000000ULL)))
    {
        memset(&g_delegation, 0, sizeof(g_delegation));
        PunchWake_ClearTemporarySuspension();
        return 1;
    }
    OwnerPut64(acknowledgement, generation);
    OwnerPut32(acknowledgement + 8, milliseconds);
    acknowledgement[12] = operation;
    return 0;
}
u32 BridgeOwner_Sign(const u8 *bytes, size_t size, u8 proof[32])
{
    char id[32] = {0}, password[64] = {0};
    UsbOwnerRequest request;
    if (!g_ready || !PunchWake_GetLocalIdentity(id, sizeof(id), password, sizeof(password)) || strcmp(id, g_id) != 0)
        return 1;
    memset(password, 0, sizeof(password));
    if (!UsbOwnerParse(bytes, size, id, &request))
        return 2;
    if (g_owner[0])
    {
        if (strlen(g_owner) != request.ownerLength || memcmp(g_owner, request.owner, request.ownerLength) != 0 ||
            strlen(g_endpoint) != request.endpointLength ||
            memcmp(g_endpoint, request.endpoint, request.endpointLength) != 0 || request.generation < g_generation)
            return 2;
    }
    else
    {
        if (request.action != 0)
            return 2;
        memcpy(g_owner, request.owner, request.ownerLength);
        g_owner[request.ownerLength] = 0;
        memcpy(g_endpoint, request.endpoint, request.endpointLength);
        g_endpoint[request.endpointLength] = 0;
    }
    g_generation = request.generation;
    hmacSha256CalculateMac(proof, g_key, sizeof(g_key), bytes, size);
    return 0;
}
// ---- first-use key setup (USB operations 8 and 9) -------------------------------------------------
// The desktop generates the key (this sysmodule has no cryptographic RNG), stages it here, enrolls it
// with the rendezvous server, and only then asks us to commit. Until the commit nothing about the
// console's ordinary hosting changes, so an interrupted setup is harmless and simply starts over.
static void PutLe32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}
static void PutLe16(u8 *p, u16 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}
static bool ValidIdBytes(const u8 *id)
{
    for (int i = 0; i < 5; i++)
        if (id[i] == 0 || !strchr("23456789ABCDEFGHJKLMNPQRSTUVWXYZ", id[i]))
            return false;
    return true;
}
static bool CurrentIdMatches(const u8 *id)
{
    char local[32] = {0}, password[64] = {0};
    bool ok = PunchWake_GetLocalIdentity(local, sizeof(local), password, sizeof(password));
    memset(password, 0, sizeof(password));
    return ok && strlen(local) == 5 && memcmp(local, id, 5) == 0;
}
static bool HasKeyFor(const u8 *id)
{
    return g_ready && memcmp(g_id, id, 5) == 0;
}

bool BridgeOwner_CanEnroll(void)
{
    return g_sd != NULL;
}

u32 BridgeOwner_StageKey(const u8 *payload, size_t size, u8 fingerprint[32])
{
    memset(fingerprint, 0, 32);
    if (!g_sd || size != 37)
        return 1;
    if (!ValidIdBytes(payload) || !CurrentIdMatches(payload))
        return 2;
    if (HasKeyFor(payload))
        return 3;
    memcpy(g_stageId, payload, 5);
    g_stageId[5] = 0;
    memcpy(g_stageKey, payload + 5, 32);
    g_staged = true;
    sha256CalculateHash(fingerprint, g_stageKey, sizeof(g_stageKey));
    return 0;
}

static bool WriteKeyFile(const u8 record[48])
{
    const char *temporary = "/config/nxremoteplay/bridge_owner.tmp";
    FsFile file;
    fsFsDeleteFile(g_sd, temporary);
    if (R_FAILED(fsFsCreateFile(g_sd, temporary, 48, 0)))
        return false;
    if (R_FAILED(fsFsOpenFile(g_sd, temporary, FsOpenMode_Write, &file)))
    {
        fsFsDeleteFile(g_sd, temporary);
        return false;
    }
    Result rc = fsFileWrite(&file, 0, record, 48, FsWriteOption_Flush);
    fsFileClose(&file);
    if (R_FAILED(rc))
    {
        fsFsDeleteFile(g_sd, temporary);
        return false;
    }
    // A stale key bound to an older ID may be here; replace it. Losing power between the delete and
    // the rename only leaves the console unprovisioned, which the next attachment repairs.
    fsFsDeleteFile(g_sd, OWNER_KEY_PATH);
    if (R_FAILED(fsFsRenameFile(g_sd, temporary, OWNER_KEY_PATH)))
    {
        fsFsDeleteFile(g_sd, temporary);
        return false;
    }
    return true;
}

u32 BridgeOwner_CommitKey(const u8 *payload, size_t size, u8 fingerprint[32])
{
    memset(fingerprint, 0, 32);
    if (!g_sd || size != 5)
        return 1;
    if (!ValidIdBytes(payload) || !CurrentIdMatches(payload))
        return 2;
    if (HasKeyFor(payload))
        return 3;
    if (!g_staged || memcmp(g_stageId, payload, 5) != 0)
        return 5;
    u8 record[48] = {0};
    PutLe32(record, USB_OWNER_KEY_MAGIC);
    PutLe16(record + 4, 1);
    PutLe16(record + 6, sizeof(record));
    memcpy(record + 8, g_stageId, 5);
    memcpy(record + 16, g_stageKey, 32);
    bool written = WriteKeyFile(record);
    memset(record, 0, sizeof(record));
    if (!written)
        return 4;
    // Load it live, then let ordinary hosting use it. Nothing here needs a reboot.
    memcpy(g_id, g_stageId, 6);
    memcpy(g_key, g_stageKey, 32);
    sha256CalculateHash(g_fingerprint, g_key, sizeof(g_key));
    g_staged = false;
    memset(g_stageKey, 0, sizeof(g_stageKey));
    memset(g_stageId, 0, sizeof(g_stageId));
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    g_ready = true;
    memcpy(fingerprint, g_fingerprint, 32);
    PunchWake_SetDirectOwnership(true);
    return 0;
}

void BridgeOwner_Remove(void)
{
    if (g_sd)
        fsFsDeleteFile(g_sd, OWNER_KEY_PATH);
    g_ready = false;
    memset(g_key, 0, sizeof(g_key));
    memset(g_fingerprint, 0, sizeof(g_fingerprint));
    memset(g_id, 0, sizeof(g_id));
    g_staged = false;
    memset(g_stageKey, 0, sizeof(g_stageKey));
    memset(g_stageId, 0, sizeof(g_stageId));
}
