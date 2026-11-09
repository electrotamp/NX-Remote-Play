#include "PunchWake.h"
#include "modes.h"
#include "PunchProbe.h"
#include "BridgeOwner.h"
#include "DirectOwnerProtocol.h"
#include "../net/sockets.h"
#include "../net/server_config.h"
#include <strings.h>
#include "../third_party/nanoprintf.h"
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>

// Rendezvous registration persists the server-issued ID/password and resumes it with
// three-argument REGISTER. Periodic REFRESH replies can request a wake.
// Renew credentials only on explicit request or identity expiry. Open and close the
// time service around each query, flush identity writes and avoid %llu in nanoprintf.
// Use short-lived UDP exchanges rather than a persistent signaling socket.

#define IDENTITY_PATH "/config/nxremoteplay/session_identity.txt"
#define IDENTITY_MAX_AGE_SECS (60ULL * 24 * 60 * 60)

// Wake latency is bounded by this. Well inside the server's 5 minute session TTL.
// was 30000. A phone's PUNCHWAKE only sets a flag on the server; the console learns of it on
// its next REFRESH, so this IS the wake latency (avg half of it). 10 s => avg ~5 s, worst ~10 s. Server side:
// sessions expire after 5 min and REFRESH is limited to 10 per 10 s per public IP (this console uses 1 per 10 s).
#define REFRESH_INTERVAL_MS 10000
#define POLL_MS 250
#define REPLY_TIMEOUT_MS 5000
#define REPLY_BUF_MAX 64
#define CRED_MAX 6 // 5 chars + NUL

static char g_relayHost[64];
static u16 g_relayPort;
static FsFileSystem *g_sdCard;

static char g_sessionId[CRED_MAX];
static char g_password[CRED_MAX];
static bool g_haveSession = false;

static char g_persistedId[CRED_MAX];
static char g_persistedPassword[CRED_MAX];
static u64 g_persistedCreatedUtc = 0;
static bool g_havePersistedIdentity = false;

static atomic_bool g_forceNewNextRegister = false;
static Mutex g_idMutex;
static atomic_ullong g_temporaryDeadline = 0;
static atomic_bool g_quiesced = false;
static u64 g_directGeneration, g_directDeadline;
static char g_directTicket[65];     // guarded by g_idMutex; one use
static atomic_bool g_directEnabled; // written by the USB thread at a first-use key commit
bool PunchWake_DirectOwnershipEnabled(void)
{
    return atomic_load(&g_directEnabled);
}
void PunchWake_SetDirectOwnership(bool enabled)
{
    atomic_store(&g_directEnabled, enabled);
}
bool PunchWake_IsTemporarilySuspended(void)
{
    u64 deadline = atomic_load(&g_temporaryDeadline);
    return deadline && svcGetSystemTick() < deadline;
}
bool PunchWake_TemporarilySuspend(u64 deadline)
{
    if (deadline <= svcGetSystemTick())
        return false;
    if (!PunchWake_IsTemporarilySuspended())
        atomic_store(&g_quiesced, false);
    atomic_store(&g_temporaryDeadline, deadline);
    u64 start = svcGetSystemTick();
    while (!atomic_load(&g_quiesced) && svcGetSystemTick() - start < armNsToTicks(2000000000ULL))
        svcSleepThread(10000000ULL);
    return PunchWake_IsTemporarilySuspended() && atomic_load(&g_quiesced);
}
void PunchWake_ClearTemporarySuspension(void)
{
    atomic_store(&g_temporaryDeadline, 0);
    atomic_store(&g_quiesced, false);
}
bool PunchWake_GetDirectTicket(char *ticket, size_t size)
{
    if (size < 65 || PunchWake_IsTemporarilySuspended())
        return false;
    mutexLock(&g_idMutex);
    bool ok = g_directTicket[0] && svcGetSystemTick() < g_directDeadline;
    if (ok)
    {
        memcpy(ticket, g_directTicket, 65);
        memset(g_directTicket, 0, sizeof(g_directTicket));
    }
    mutexUnlock(&g_idMutex);
    return ok;
}

// Notice line for the NRO's Host screen status line (guarded by g_idMutex): raised when the
// identity was changed automatically (id taken on the server, aged out after 60 days, minted locally).
static char g_noticeText[96];
static u64 g_noticeTick = 0;

// Server change requested from the NRO (PunchWake_SetServer): stored here under g_idMutex, applied
// by the session thread at the top of its loop (so it never changes under an in-flight exchange).
static atomic_bool g_serverChanged = false;
static char g_pendingHost[64];
static u16 g_pendingPort;

// NRO-facing hosting state for CMD_GET_HOSTING_STATE.
#define ENABLED_STATE_PATH "/config/nxremoteplay/hosting_enabled.txt"
static atomic_bool g_enabled = true;
static HostingSnapshot g_snapshot;
static void SetSnapshot(HostingState state, const char *errorMessage)
{
    mutexLock(&g_idMutex);
    g_snapshot.state = state;
    // The current Session ID/Password are always reported when we have one - including while the
    // server is unreachable (Error) - because the pair is still valid for connections on the local
    // network; the Host screen used to show only an error in that case.
    if (g_sessionId[0])
    {
        strcpy(g_snapshot.id, g_sessionId);
        strcpy(g_snapshot.password, g_password);
    }
    if (errorMessage)
    {
        strncpy(g_snapshot.errorMessage, errorMessage, sizeof(g_snapshot.errorMessage) - 1);
        g_snapshot.errorMessage[sizeof(g_snapshot.errorMessage) - 1] = 0;
    }
    mutexUnlock(&g_idMutex);
}

static u8 alignas(0x1000) PunchWakeStackArea[0x3000 + LOGGING_STACK_BOOST];
static Thread PunchWakeThreadHandle;

// ---------------------------------------------------------------- persistence

static bool ExtractValue(const char *text, const char *key, char *out, size_t outSize)
{
    const char *p = strstr(text, key);
    if (!p)
        return false;

    p += strlen(key);
    size_t i = 0;
    while (p[i] && p[i] != '\n' && p[i] != '\r' && i < outSize - 1)
    {
        out[i] = p[i];
        i++;
    }
    out[i] = 0;
    return i > 0;
}

static bool ReadPersistedIdentity(void)
{
    FsFile file;
    if (R_FAILED(fsFsOpenFile(g_sdCard, IDENTITY_PATH, FsOpenMode_Read, &file)))
        return false;

    static char buf[128];
    u64 bytesRead = 0;
    Result rc = fsFileRead(&file, 0, buf, sizeof(buf) - 1, FsReadOption_None, &bytesRead);
    fsFileClose(&file);
    if (R_FAILED(rc) || bytesRead == 0)
        return false;
    buf[bytesRead] = 0;

    char idVal[16] = {0}, pwVal[16] = {0}, createdVal[32] = {0};
    if (!ExtractValue(buf, "id=", idVal, sizeof(idVal)) || !ExtractValue(buf, "password=", pwVal, sizeof(pwVal)) ||
        !ExtractValue(buf, "created=", createdVal, sizeof(createdVal)))
    {
        return false;
    }
    if (strlen(idVal) >= sizeof(g_persistedId) || strlen(pwVal) >= sizeof(g_persistedPassword))
    {
        return false;
    }

    strcpy(g_persistedId, idVal);
    strcpy(g_persistedPassword, pwVal);
    g_persistedCreatedUtc = strtoull(createdVal, NULL, 10);
    return true;
}

// Delete-create-write with Flush: a reboot right after a new code is issued must not lose it.
static void PersistIdentity(void)
{
    fsFsDeleteFile(g_sdCard, IDENTITY_PATH);

    if (R_FAILED(fsFsCreateFile(g_sdCard, IDENTITY_PATH, 64, 0)))
    {
        return;
    }

    FsFile file;
    if (R_FAILED(fsFsOpenFile(g_sdCard, IDENTITY_PATH, FsOpenMode_Write, &file)))
    {
        return;
    }

    // %u, not %llu - nanoprintf here silently mishandles 64-bit arguments (a Unix timestamp
    // fits in 32 bits until 2106).
    char buf[80];
    int n = npf_snprintf(buf, sizeof(buf), "id=%s\npassword=%s\ncreated=%u\n", g_persistedId, g_persistedPassword,
                         (unsigned int)g_persistedCreatedUtc);
    fsFileWrite(&file, 0, buf, (u64)n, FsWriteOption_Flush);
    fsFileClose(&file);
}

// Release the time service after each query; retaining it can exhaust the session pool.
static u64 NowUtcSeconds(void)
{
    if (R_FAILED(timeInitialize()))
        return 0;
    u64 out = 0;
    Result rc = timeGetCurrentTime(TimeType_UserSystemClock, &out);
    timeExit();
    if (R_FAILED(rc))
        return 0;
    return out;
}

// Fails OPEN when age can't be measured - refusing to resume just because the clock is
// unavailable would silently reintroduce unwanted renewal.
static bool IdentityIsFresh(void)
{
    if (!g_havePersistedIdentity)
        return false;
    if (g_persistedCreatedUtc == 0)
        return true;
    u64 now = NowUtcSeconds();
    if (now == 0 || now < g_persistedCreatedUtc)
        return true;
    return (now - g_persistedCreatedUtc) < IDENTITY_MAX_AGE_SECS;
}

// ---------------------------------------------------------------- UDP exchange

// One send + bounded wait for one reply, then close. False on any failure/timeout.
static bool DoExchange(const char *message, char *reply, size_t replySize)
{
    if (PunchWake_IsTemporarilySuspended())
        return false;
    int sock = SocketUdp();
    if (sock == SOCKET_INVALID)
    {
        return false;
    }
    if (!SocketMakeNonBlocking(sock))
    {
        SocketClose(&sock);
        return false;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(g_relayPort);
    addr.sin_addr.s_addr = SocketResolveHostV4(g_relayHost);

    if (!SocketUDPSendTo(sock, message, (u32)strlen(message), (struct sockaddr *)&addr, sizeof(addr)))
    {
        SocketClose(&sock);
        return false;
    }

    bool got = false;
    int waitedMs = 0;
    while (waitedMs < REPLY_TIMEOUT_MS)
    {
        if (PunchWake_IsTemporarilySuspended())
            break;
        struct sockaddr_in from = {0};
        socklen_t fromLen = sizeof(from);
        ssize_t r = bsdRecvFrom(sock, reply, replySize - 1, 0, (struct sockaddr *)&from, &fromLen);
        if (r > 0 && from.sin_addr.s_addr == addr.sin_addr.s_addr && from.sin_port == addr.sin_port)
        {
            reply[r] = 0;
            got = true;
            break;
        }
        svcSleepThread(100ULL * 1000 * 1000); // 100ms
        waitedMs += 100;
    }
    SocketClose(&sock);
    return got;
}

static void RaiseNotice(const char *text)
{
    mutexLock(&g_idMutex);
    strncpy(g_noticeText, text, sizeof(g_noticeText) - 1);
    g_noticeText[sizeof(g_noticeText) - 1] = 0;
    g_noticeTick = svcGetSystemTick();
    mutexUnlock(&g_idMutex);
}

// Random 5-character code from the same alphabet the Rendezvous server uses (no 0/1/I/O), so the
// server accepts it as a valid id/password when it is later registered with REGISTER-reclaim.
static void RandomCode(char out[CRED_MAX])
{
    static const char alphabet[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
    static u64 counter = 0xD1B54A32D192ED03ull;
    for (int i = 0; i < 5; i++)
    {
        counter = counter * 6364136223846793005ull + svcGetSystemTick() + 1442695040888963407ull;
        out[i] = alphabet[(u32)((counter ^ (counter >> 33)) % (sizeof(alphabet) - 1))];
    }
    out[5] = 0;
}

static void PublishIdentity(void);

// Creates a brand-new Session ID/Password on this console (no server involved), persists it and
// makes it the live identity - used when there is no saved identity at all (first boot, or the file
// is gone) and when Refresh is pressed with the server unreachable. It works for connections on the
// local network immediately; once the server is reachable the normal REGISTER-reclaim registers this
// same pair there (or renews it if someone else already holds that id).
static void MintLocalIdentity(void)
{
    RandomCode(g_persistedId);
    RandomCode(g_persistedPassword);
    g_persistedCreatedUtc = NowUtcSeconds();
    g_havePersistedIdentity = true;
    PersistIdentity();

    mutexLock(&g_idMutex);
    strcpy(g_sessionId, g_persistedId);
    strcpy(g_password, g_persistedPassword);
    mutexUnlock(&g_idMutex);
    PublishIdentity();
}

static void PublishIdentity(void)
{
    PunchProbe_SetIdentity(g_sessionId, g_password);
    mutexLock(&g_idMutex);
    mutexUnlock(&g_idMutex);
}

// Fire-and-forget (no reply awaited): tells the server the instant hosting is turned off.
static void DoUnregister(void)
{
    if (PunchWake_IsTemporarilySuspended())
        return;
    char message[32];
    npf_snprintf(message, sizeof(message), "UNREGISTER %s %s", g_sessionId, g_password);

    int sock = SocketUdp();
    if (sock == SOCKET_INVALID)
        return;

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(g_relayPort);
    addr.sin_addr.s_addr = SocketResolveHostV4(g_relayHost);
    SocketUDPSendTo(sock, message, (u32)strlen(message), (struct sockaddr *)&addr, sizeof(addr));
    SocketClose(&sock);
}

static void DoRegisterCycle(void)
{
    char reply[REPLY_BUF_MAX];
    char message[32];
    SetSnapshot(HostingState_Requesting, NULL);

    // A manual "new code" always wins over resuming.
    const bool wasForced = atomic_load(&g_forceNewNextRegister);
    bool reclaiming = !wasForced && IdentityIsFresh();
    if (reclaiming)
        npf_snprintf(message, sizeof(message), "REGISTER %s %s", g_persistedId, g_persistedPassword);
    else
        strcpy(message, "REGISTER");

    if (!DoExchange(message, reply, sizeof(reply)) || strncmp(reply, "REGISTERED ", 11) != 0)
    {
        if (PunchWake_IsTemporarilySuspended())
            return;
        // Refresh was pressed but the server can't be reached: make the new pair here instead of
        // silently doing nothing. It is usable on the local network right away and gets registered
        // with the server when that is reachable again.
        if (atomic_load(&g_forceNewNextRegister))
        {
            MintLocalIdentity();
            atomic_store(&g_forceNewNextRegister, false);
            RaiseNotice(
                "The server can't be reached, so a new ID was created on this console. It works on your local network now.");
        }
        SetSnapshot(HostingState_Error, "Could not reach the rendezvous server");
        return;
    }

    char *idStart = reply + 11;
    if (PunchWake_IsTemporarilySuspended())
        return;
    char *space = strchr(idStart, ' ');
    if (!space || (size_t)(space - idStart) >= sizeof(g_sessionId))
    {
        SetSnapshot(HostingState_Error, "Unexpected server reply");
        return;
    }

    size_t idLen = (size_t)(space - idStart);
    memcpy(g_sessionId, idStart, idLen);
    g_sessionId[idLen] = 0;
    strncpy(g_password, space + 1, sizeof(g_password) - 1);
    g_password[sizeof(g_password) - 1] = 0;
    g_haveSession = true;

    // The server only returns something other than what we asked to resume if that id was
    // taken (or we asked for a new one on purpose): a genuinely new identity, so its 60-day
    // clock starts now. A successful resume must NOT reset the clock.
    bool same = reclaiming && strcmp(g_sessionId, g_persistedId) == 0 && strcmp(g_password, g_persistedPassword) == 0;
    if (!same)
    {
        // Say WHY the pair changed when nobody asked for it (a Refresh press is its own explanation).
        if (reclaiming)
            RaiseNotice(
                "Your Session ID was already taken on the server, so a new ID and password were issued automatically.");
        else if (g_havePersistedIdentity && !wasForced)
            RaiseNotice(
                "Your Session ID was more than 60 days old, so a new ID and password were issued automatically.");

        strcpy(g_persistedId, g_sessionId);
        strcpy(g_persistedPassword, g_password);
        g_persistedCreatedUtc = NowUtcSeconds();
        g_havePersistedIdentity = true;
        PersistIdentity();
    }
    else if (g_persistedCreatedUtc == 0 && NowUtcSeconds() != 0)
    {
        g_persistedCreatedUtc = NowUtcSeconds();
        PersistIdentity();
    }

    atomic_store(&g_forceNewNextRegister, false);
    PublishIdentity();
    SetSnapshot(HostingState_Registered, NULL);
}

static void DoRefreshCycle(void)
{
    char reply[REPLY_BUF_MAX];
    char message[32];
    npf_snprintf(message, sizeof(message), "REFRESH %s %s", g_sessionId, g_password);

    if (!DoExchange(message, reply, sizeof(reply)))
    {
        g_haveSession = false;
        return;
    }
    if (PunchWake_IsTemporarilySuspended())
        return;

    if (strncmp(reply, "WAKE", 4) == 0)
    {
        PunchProbe_RequestWake();
    }
    else if (strncmp(reply, "OK", 2) != 0)
    {
        g_haveSession = false;
    }
}

static size_t DirectExchange(int socket, struct sockaddr_in *address, const u8 *request, size_t size,
                             u8 reply[USB_OWNER_MAX + 64])
{
    if (PunchWake_IsTemporarilySuspended() ||
        !SocketUDPSendTo(socket, request, size, (struct sockaddr *)address, sizeof(*address)))
        return 0;
    u64 start = svcGetSystemTick();
    while (!PunchWake_IsTemporarilySuspended() && svcGetSystemTick() - start < armNsToTicks(2000000000ULL))
    {
        struct sockaddr_in from = {0};
        socklen_t length = sizeof(from);
        ssize_t received = bsdRecvFrom(socket, reply, USB_OWNER_MAX + 64, 0, (struct sockaddr *)&from, &length);
        if (received > 0 && from.sin_addr.s_addr == address->sin_addr.s_addr && from.sin_port == address->sin_port)
            return received;
        svcSleepThread(10000000ULL);
    }
    return 0;
}
static bool DirectCycle(u8 action)
{
    if (PunchWake_IsTemporarilySuspended() || !BridgeOwner_IsProvisioned(g_sessionId))
        return false;
    char secret[12], endpoint[80] = "nxrp-direct-v1:";
    memcpy(secret, g_sessionId, 5);
    secret[5] = '\n';
    memcpy(secret + 6, g_password, 5);
    u8 verifier[32];
    sha256CalculateHash(verifier, secret, 11);
    memset(secret, 0, sizeof(secret));
    const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 32; i++)
    {
        endpoint[15 + i * 2] = hex[verifier[i] >> 4];
        endpoint[16 + i * 2] = hex[verifier[i] & 15];
    }
    endpoint[79] = 0;
    u64 correlation = svcGetSystemTick();
    if (!correlation)
        correlation = 1;
    u8 request[128], reply[USB_OWNER_MAX + 64];
    DirectOwnerHeader(request, 64, g_sessionId, 1, action, g_directGeneration, correlation);
    memcpy(request + 32, verifier, 32);
    int socket = SocketUdp();
    if (socket == SOCKET_INVALID)
        return false;
    if (!SocketMakeNonBlocking(socket))
    {
        SocketClose(&socket);
        return false;
    }
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(g_relayPort);
    address.sin_addr.s_addr = SocketResolveHostV4(g_relayHost);
    size_t size = DirectExchange(socket, &address, request, 64, reply);
    bool success = false;
    if (DirectOwnerReplyValid(reply, size, g_sessionId, 2, action, correlation) && !reply[7] &&
        BridgeOwner_VerifyDirectReply(reply, size))
    {
        u64 expected = OwnerU64(reply + 16);
        UsbOwnerRequest parsed;
        if (OwnerParse(reply + 32, size - 64, g_sessionId, 0, &parsed) && parsed.generation == expected)
        {
            DirectOwnerHeader(request, sizeof(request), g_sessionId, 3, action, expected, correlation);
            memcpy(request + 32, reply + size - 104, 64); // canonical nonce precedes expiry and response MAC
            if (BridgeOwner_SignDirect(reply + 32, size - 64, endpoint, action, request + 96))
            {
                size = DirectExchange(socket, &address, request, sizeof(request), reply);
                if (DirectOwnerReplyValid(reply, size, g_sessionId, 4, action, correlation) && !reply[7] &&
                    size == 136 && BridgeOwner_VerifyDirectReply(reply, size) &&
                    (action == 1 ? OwnerU64(reply + 16) == expected : OwnerU64(reply + 16) > expected))
                {
                    u32 lifetime = OwnerU32(reply + 32);
                    if ((action == 2 && !lifetime) || (action != 2 && lifetime && lifetime <= 15000))
                    {
                        g_directGeneration = OwnerU64(reply + 16);
                        mutexLock(&g_idMutex);
                        g_directDeadline = svcGetSystemTick() + armNsToTicks((u64)lifetime * 1000000ULL);
                        if (action != 2)
                        {
                            memcpy(g_directTicket, reply + 40, 64);
                            g_directTicket[64] = 0;
                        }
                        else
                            memset(g_directTicket, 0, sizeof(g_directTicket));
                        mutexUnlock(&g_idMutex);
                        success = true;
                        if (action != 2 && reply[36] == 1 && !PunchWake_IsTemporarilySuspended())
                        {
                            NetworkSupportLine("authenticated wake received");
                            PunchProbe_RequestWake();
                        }
                    }
                }
            }
        }
    }
    memset(request, 0, sizeof(request));
    SocketClose(&socket);
    static u64 nextReportTick;
    const u64 now = svcGetSystemTick();
    if (now >= nextReportTick)
    {
        NetworkSupportLine("authenticated hosting check-in: %s", success ? "accepted" : "failed");
        nextReportTick = now + armNsToTicks(30000000000ULL);
    }
    return success;
}

// Applies the shared server file (written only by the NRO). Called every cycle: when the file selects a different
// server than the one in use, switch to it the same way an IPC SetServer does (leave the old, re-register at the
// new one with the same Session ID/Password). The file is the source of truth.
static void CheckServerFile(void)
{
    char eff[SERVERCFG_TEXT_MAX];
    char host[64];
    unsigned port = 0;
    if (atomic_load(&g_serverChanged) || !g_sdCard)
        return;
    if (!PunchProbe_ReadServerFile(g_sdCard, eff, sizeof(eff)) || !ServerCfg_Split(eff, host, sizeof(host), &port))
        return;
    mutexLock(&g_idMutex);
    const bool same = strcasecmp(host, g_relayHost) == 0 && port == (unsigned)g_relayPort;
    if (!same)
    {
        strcpy(g_pendingHost, host);
        g_pendingPort = (u16)port;
    }
    mutexUnlock(&g_idMutex);
    if (same)
        return;
    atomic_store(&g_serverChanged, true);
}

static void PunchWakeThread(void *arg)
{
    (void)arg;

    while (true)
    {
        if (PunchWake_IsTemporarilySuspended())
        {
            g_haveSession = false;
            g_directGeneration = 0;
            mutexLock(&g_idMutex);
            memset(g_directTicket, 0, sizeof(g_directTicket));
            mutexUnlock(&g_idMutex);
            atomic_store(&g_quiesced, true);
            svcSleepThread(POLL_MS * 1000000ULL);
            continue;
        }
        atomic_store(&g_quiesced, false);
        CheckServerFile(); // the shared server.txt (NRO-owned) is applied every cycle, also for protected hosts
        // A new code (Refresh, or the 60-day age-out) belongs to a new Session ID, and the owner key is bound to
        // the old one. Release the old route, forget the key and carry on through the ordinary registration
        // below, which mints the new code; the next USB attachment enrolls a key for it automatically.
        if (atomic_load(&g_directEnabled) && (atomic_load(&g_forceNewNextRegister) || !IdentityIsFresh()))
        {
            if (g_directGeneration)
                DirectCycle(2);
            g_directGeneration = 0;
            BridgeOwner_Remove();
            atomic_store(&g_directEnabled, false);
            g_haveSession = false;
        }
        if (atomic_load(&g_directEnabled))
        {
            if (atomic_exchange(&g_serverChanged, false))
            {
                if (g_directGeneration)
                    DirectCycle(2);
                mutexLock(&g_idMutex);
                strcpy(g_relayHost, g_pendingHost);
                g_relayPort = g_pendingPort;
                mutexUnlock(&g_idMutex);
                PunchProbe_SetRelay(g_relayHost, g_relayPort);
                g_directGeneration = 0;
            }
            if (!atomic_load(&g_enabled))
            {
                if (g_directGeneration)
                    DirectCycle(2);
                g_directGeneration = 0;
                PunchProbe_SetIdentity("", "");
                SetSnapshot(HostingState_Disabled, NULL);
            }
            else
            {
                PunchProbe_SetIdentity(g_sessionId, g_password);
                bool ok = DirectCycle(g_directGeneration ? 1 : 0);
                if (!ok)
                    g_directGeneration = 0;
                SetSnapshot(ok ? HostingState_Registered : HostingState_Error,
                            ok ? NULL : "Authenticated hosting unavailable");
            }
            for (u32 waited = 0; waited < REFRESH_INTERVAL_MS; waited += POLL_MS)
            {
                svcSleepThread(POLL_MS * 1000000ULL);
                if (PunchWake_IsTemporarilySuspended() || atomic_load(&g_serverChanged) ||
                    atomic_load(&g_forceNewNextRegister))
                    break;
            }
            continue;
        }
        // Server changed from the NRO: leave the old server (best effort), switch, and
        // re-register at the new one - REGISTER-reclaim resumes the same Session ID/Password.
        if (atomic_exchange(&g_serverChanged, false))
        {
            if (g_haveSession)
                DoUnregister();
            mutexLock(&g_idMutex);
            strncpy(g_relayHost, g_pendingHost, sizeof(g_relayHost) - 1);
            g_relayHost[sizeof(g_relayHost) - 1] = 0;
            g_relayPort = g_pendingPort;
            mutexUnlock(&g_idMutex);
            PunchProbe_SetRelay(g_relayHost, g_relayPort);
            g_haveSession = false;
        }

        if (!atomic_load(&g_enabled))
        {
            // Hosting turned off from the NRO: no REGISTER/REFRESH at all. Tell the server
            // immediately (once), withdraw the identity from PunchProbe so no punch can start,
            // then poll until re-enabled.
            if (g_haveSession)
            {
                DoUnregister();
                g_haveSession = false;
            }
            PunchProbe_SetIdentity("", "");
            mutexLock(&g_idMutex);
            mutexUnlock(&g_idMutex);
            SetSnapshot(HostingState_Disabled, NULL);
            while (!atomic_load(&g_enabled) && !PunchWake_IsTemporarilySuspended())
                svcSleepThread(POLL_MS * 1000000ULL);
            continue;
        }

        if (!g_haveSession || atomic_load(&g_forceNewNextRegister))
            DoRegisterCycle();
        else
            DoRefreshCycle();

        // Short polls so a "new code" request is picked up promptly.
        for (u32 waited = 0; waited < REFRESH_INTERVAL_MS; waited += POLL_MS)
        {
            svcSleepThread(POLL_MS * 1000000ULL);
            if (PunchWake_IsTemporarilySuspended() || atomic_load(&g_forceNewNextRegister) ||
                !atomic_load(&g_enabled) || atomic_load(&g_serverChanged))
                break;
        }
    }
}

void PunchWake_Start(FsFileSystem *sdCard)
{
    char id[32];
    char password[64];
    if (!PunchProbe_GetRelayConfig(g_relayHost, sizeof(g_relayHost), &g_relayPort, id, sizeof(id), password,
                                   sizeof(password)))
    {
        return;
    }

    g_sdCard = sdCard;
    mutexInit(&g_idMutex);
    g_snapshot.state = HostingState_Idle;
    {
        // Absent file = on (the default). A leading '0' = the user turned hosting off.
        FsFile file;
        bool on = true;
        if (R_SUCCEEDED(fsFsOpenFile(sdCard, ENABLED_STATE_PATH, FsOpenMode_Read, &file)))
        {
            char buf[4] = {0};
            u64 bytesRead = 0;
            fsFileRead(&file, 0, buf, sizeof(buf) - 1, FsReadOption_None, &bytesRead);
            fsFileClose(&file);
            on = bytesRead == 0 || buf[0] != '0';
        }
        atomic_store(&g_enabled, on);
    }
    g_havePersistedIdentity = ReadPersistedIdentity();
    // A provisioned console supports automatic USB host  by default.
    // Delegation still requires a valid authenticated server lease and USB commit;
    // merely plugging in USB does not suppress hosting on an unverified route.
    atomic_store(&g_directEnabled, g_havePersistedIdentity && BridgeOwner_IsProvisioned(g_persistedId));
    // LAN discovery (PunchProbe.c's LanHost) needs the Session ID/Password even when the server
    // is unreachable, so publish the persisted identity right away instead of waiting for the
    // first successful REGISTER (which then re-publishes the same values, or a new pair).
    if (g_havePersistedIdentity)
    {
        strcpy(g_sessionId, g_persistedId);
        strcpy(g_password, g_persistedPassword);
        if (atomic_load(&g_enabled))
            PunchProbe_SetIdentity(g_persistedId, g_persistedPassword);
    }
    else
    {
        // No saved identity at all (first boot, or the file is gone). The server is not the only
        // source of IDs any more: make one here so local connections work even if the server
        // never answers; it is registered with the server (reclaim) whenever that is reachable.
        MintLocalIdentity();
        if (!atomic_load(&g_enabled))
            PunchProbe_SetIdentity("", "");
    }
    g_snapshot.state = HostingState_Idle;
    if (g_sessionId[0])
    {
        strcpy(g_snapshot.id, g_sessionId);
        strcpy(g_snapshot.password, g_password);
    }

    LaunchThread(&PunchWakeThreadHandle, PunchWakeThread, NULL, PunchWakeStackArea, sizeof(PunchWakeStackArea), 0x2C);
}

bool PunchWake_GetLocalIdentity(char *outId, size_t idSize, char *outPassword, size_t passwordSize)
{
    if (idSize == 0 || passwordSize == 0)
        return false;
    mutexLock(&g_idMutex);
    bool ok = g_sessionId[0] != 0 && g_password[0] != 0;
    if (ok)
    {
        strncpy(outId, g_sessionId, idSize - 1);
        outId[idSize - 1] = 0;
        strncpy(outPassword, g_password, passwordSize - 1);
        outPassword[passwordSize - 1] = 0;
    }
    mutexUnlock(&g_idMutex);
    return ok;
}

// Explicit user action - the one case (besides 60-day age-out) allowed to mint a new code.
void PunchWake_RequestNewCode(void)
{
    atomic_store(&g_forceNewNextRegister, true);
}

void PunchWake_GetServer(char *out, size_t outSize)
{
    mutexLock(&g_idMutex);
    const bool haveOwn = g_relayHost[0] != 0 && g_relayPort != 0;
    if (haveOwn)
        npf_snprintf(out, outSize, "%s:%u", g_relayHost, (unsigned)g_relayPort);
    mutexUnlock(&g_idMutex);
    if (haveOwn)
        return;

    // PunchWake_Start never ran (no punch_wake.txt on this console), so g_relayHost is empty -
    // this used to report ":0", which the NRO took as the server and then pointed its footer
    // probe and Connect at nothing (, .198). Report the server PunchProbe loaded
    // (server.txt or the built-in default), or an empty string if there is none.
    char host[64], id[32], pw[64];
    u16 port = 0;
    if (PunchProbe_GetRelayConfig(host, sizeof(host), &port, id, sizeof(id), pw, sizeof(pw)) && host[0] && port)
        npf_snprintf(out, outSize, "%s:%u", host, (unsigned)port);
    else
        out[0] = 0;
}

// Accepts "host:port" (or a bare host, default port 7921). Rejects anything without a plain
// hostname/IPv4 character set so a stray URL/space can't corrupt the shared server.txt.
bool PunchWake_SetServer(const char *hostPort)
{
    char host[64];
    strncpy(host, hostPort, sizeof(host) - 1);
    host[sizeof(host) - 1] = 0;

    unsigned port = 7921;
    char *colon = strrchr(host, ':');
    if (colon)
    {
        *colon = 0;
        char *end = NULL;
        unsigned long p = strtoul(colon + 1, &end, 10);
        if (end == colon + 1 || *end != 0 || p < 1 || p > 65535)
            return false;
        port = (unsigned)p;
    }
    if (host[0] == 0)
        return false;
    // Hostnames are resolved on use (SocketResolveHostV4); only reject characters that cannot be a name.
    for (const char *c = host; *c; c++)
        if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || *c == '.' ||
              *c == '-'))
            return false;

    mutexLock(&g_idMutex);
    strcpy(g_pendingHost, host);
    g_pendingPort = (u16)port;
    mutexUnlock(&g_idMutex);

    // Not persisted here: the shared /config/nxremoteplay/server.txt (written only by the NRO) is the source of truth;
    // this IPC call just makes the running sysmodule apply it immediately.
    atomic_store(&g_serverChanged, true);
    return true;
}

void PunchWake_GetSnapshot(HostingSnapshot *out)
{
    mutexLock(&g_idMutex);
    *out = g_snapshot;
    out->notice[0] = 0;
    out->noticeAgeSec = 0;
    if (g_noticeText[0])
    {
        strncpy(out->notice, g_noticeText, sizeof(out->notice) - 1);
        out->notice[sizeof(out->notice) - 1] = 0;
        out->noticeAgeSec = (unsigned int)(armTicksToNs(svcGetSystemTick() - g_noticeTick) / 1000000000ull);
    }
    mutexUnlock(&g_idMutex);
}

void PunchWake_SetEnabled(bool enabled)
{
    atomic_store(&g_enabled, enabled);
    if (!g_sdCard)
        return;

    // Flush is required - a reboot right after toggling must not lose the choice.
    fsFsDeleteFile(g_sdCard, ENABLED_STATE_PATH);
    if (R_FAILED(fsFsCreateFile(g_sdCard, ENABLED_STATE_PATH, 1, 0)))
        return;
    FsFile file;
    if (R_FAILED(fsFsOpenFile(g_sdCard, ENABLED_STATE_PATH, FsOpenMode_Write, &file)))
        return;
    const char value = enabled ? '1' : '0';
    fsFileWrite(&file, 0, &value, 1, FsWriteOption_Flush);
    fsFileClose(&file);
}

