#include "PunchProbe.h"
#include "PunchWake.h"
#include "modes.h"
#include "../net/sockets.h"
#include "../net/server_config.h"
#include "../capture.h"
#include "../nxrp_video.h"
#include "../hidplus/udp_manager.hpp"
#include "proto.h"
#include "ClientInput.h"
#include "Viewers.h"
#include "UdpmVideo.h"
#include "../third_party/nanoprintf.h"
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <unistd.h> // gethostid (same call hidplus/udp_manager.cpp already links)
#include <switch/crypto/sha256.h>

// See PunchProbe.h for exactly what this does and doesn't test yet.
//
// second pass: was a fixed-target mechanics-only probe (proved bind-from-
// explicit-local-port works at all - see the git history / this session's notes for that
// result). Now does the real relay-assisted flow: register with PunchRelayCore (PC-side,
// RelayServer/PunchRelayCore.cs), wait for it to push the peer's observed address down the
// same connection, then immediately close that connection and attempt the actual punch
// from the SAME local port toward the peer. Whether the punch itself succeeds depends on
// both sides' NAT behavior - this file's job is just to do its half of the dance correctly.

#define PROBE_HOST_MAX 64
#define PROBE_ID_MAX 32
#define PROBE_PASSWORD_MAX 64
#define PROBE_LINE_MAX 96

// Same bounded-window value RelayProbe.c already proved safe - reused deliberately, not
// re-derived, since "a single attempt, bounded to this long, then give up" is exactly the
// property that's already hardware-confirmed not to interact badly with sleep/Restart.
// was 60000. The phone keeps trying PUNCHJOIN for 45 s (PunchSession.kt joinDeadline), so waiting much
// longer than that only keeps this console "busy" for nobody; 50 s = the phone's window + slack.
#define PUNCH_PROBE_WAIT_MS 50000
#define PUNCH_PROBE_WAIT_CHECK_MS 200

// Y held continuously for this long on the home menu triggers a probe attempt. Deliberately
// a different button than RelayProbe.c's B, so the two probes can never be confused for one
// another if both happen to be configured at once.
#define TRIGGER_HOLD_NS (5ULL * 1000 * 1000 * 1000)
#define GAME_FOCUSED_WINDOW_NS (2ULL * 1000 * 1000 * 1000)

// Dedicated test port - deliberately NOT 9911/9922 (TCP_MODE's real video/audio ports).
// A failure or even a crash in this probe's socket handling must never be able to affect
// real streaming; using an unrelated port makes that a structural guarantee, not just a
// hoped-for outcome. Also the port the relay observes and the port the real punch attempt
// connects FROM - they must be the same port for the punch to have any chance of working.

static u16 UdpMapEnsure(u16 internal);
static char g_relayHost[PROBE_HOST_MAX];
static u16 g_relayPort;
static char g_id[PROBE_ID_MAX];
static char g_password[PROBE_PASSWORD_MAX];

bool PunchProbe_ReadServerFile(FsFileSystem *sd, char *effOut, size_t effSize)
{
    FsFile file;
    if (!sd || R_FAILED(fsFsOpenFile(sd, SERVERCFG_PATH, FsOpenMode_Read, &file)))
        return false;
    char buf[320];
    u64 n = 0;
    const Result rc = fsFileRead(&file, 0, buf, sizeof(buf) - 1, FsReadOption_None, &n);
    fsFileClose(&file);
    if (R_FAILED(rc) || n == 0)
        return false;
    buf[n] = 0;
    ServerConfig c;
    if (!ServerCfg_Parse(buf, &c))
        return false;
    const char *e = ServerCfg_Effective(&c);
    if (strlen(e) + 1 > effSize)
        return false;
    strcpy(effOut, e);
    return true;
}

// The rendezvous server: the NRO's shared server.txt when present, otherwise the built-in default. Always succeeds.
static bool ReadConfig(FsFileSystem *sdCard)
{
    char eff[SERVERCFG_TEXT_MAX];
    strcpy(eff, SERVERCFG_BUILTIN_DEFAULT);
    if (!PunchProbe_ReadServerFile(sdCard, eff, sizeof(eff)))
        strcpy(eff, SERVERCFG_BUILTIN_DEFAULT);

    unsigned port = 0;
    if (!ServerCfg_Split(eff, g_relayHost, sizeof(g_relayHost), &port))
    {
        return false;
    }
    g_relayPort = (u16)port;

    // id/password are not configured here: the server issues them and PunchWake.c registers and persists the
    // identity (/config/nxremoteplay/session_identity.txt), then hands it over via PunchProbe_SetIdentity.
    g_id[0] = 0;
    g_password[0] = 0;

    return true;
}

// Blocking (via sleep-poll) read of a single \n-terminated line, bounded by timeoutMs.
// Same shape as RelayProbe.c's own ReadLine - duplicated rather than shared, matching this
// codebase's established probe convention.
static bool ReadLine(int socket, char *buf, size_t maxLen, int timeoutMs)
{
    size_t i = 0;
    int waitedMs = 0;

    while (i < maxLen - 1)
    {
        char c;
        s32 r = SocketRecv(socket, &c, 1);

        if (r < 0)
            return false;

        if (r == 0)
        {
            svcSleepThread(1E+6); // 1ms
            waitedMs++;
            if (waitedMs >= timeoutMs)
                return false;
            continue;
        }

        if (c == '\n')
        {
            buf[i] = 0;
            return true;
        }

        buf[i++] = c;
    }

    buf[i] = 0;
    return true;
}

// Parses "PEER a.b.c.d:port" into separate host/port strings. Returns false on any
// malformed input - the caller treats that as a failed attempt, no partial/guessed punch.
static bool ParsePeerLine(const char *line, char *outHost, size_t outHostSize, u16 *outPort)
{
    if (strncmp(line, "PEER ", 5) != 0)
        return false;

    const char *addr = line + 5;
    const char *colon = strrchr(addr, ':');
    if (!colon)
        return false;

    size_t hostLen = (size_t)(colon - addr);
    if (hostLen == 0 || hostLen >= outHostSize)
        return false;

    memcpy(outHost, addr, hostLen);
    outHost[hostLen] = 0;

    int port = atoi(colon + 1);
    if (port <= 0 || port > 65535)
        return false;

    *outPort = (u16)port;
    return true;
}

//  (multi-viewer): the single "probe in progress" flag became a set of viewer places (see Viewers.h). USB is
// still the exclusive owner of capture while attached: it can attach only when no viewer session exists.
bool PunchProbe_TryAcquireUsb(void)
{
    return Viewers_UsbTryAcquire();
}

void PunchProbe_ReleaseUsb(void)
{
    Viewers_UsbRelease();
}
// true only while a server punch is merely WAITING for a connector to show up (before any peer
// exists). A LAN DISCOVER arriving then cancels that wait (g_cancelPunchWait) and takes over instead of being
// told "busy" for up to a minute.
static atomic_bool g_punchWaiting = false;
static atomic_bool g_cancelPunchWait = false;

void PunchProbe_EvictViewersForUsb(void)
{
    Viewers_KickAll();
    if (atomic_load(&g_punchWaiting))
        atomic_store(&g_cancelPunchWait, true);
}

//  (multi-viewer): controller/touch/HOME input polling now lives with the viewer (Viewers.c:
// Viewers_ServiceInput, also installed as the send helper's readable hook), because every viewer has its own socket,
// partial-packet buffer and pad slot.

// Limit each audio batch to preserve opportunities to send video.
static bool SendPendingAudio(Viewer *v, u32 *packets, u64 *bytes, bool udp)
{
    const u64 begin = svcGetSystemTick();
    for (int i = 0; i < 8; i++)
    {
        const u8 *wire = NULL;
        u32 size = 0;
        if (!Viewers_NextAudio(v, &wire, &size))
            break;
        // With UDP media the audio travels with the video as UDPM audio frames; the TCP connection carries none.
        if (!(udp                    ? UdpmVideo_SendAudio(wire, size)
              : (Viewers_VideoSend(v, wire, size, false))))
            return false;
        (*packets)++;
        *bytes += size;
        break; // one complete PCM packet, then video opportunity
    }
    if (udp)
        UdpmVideo_RecordLoopTime(armTicksToNs(svcGetSystemTick() - begin) / 1000, 0);
    return true;
}

// Waits up to timeoutMs for exactly size bytes; false on timeout/close. Used for connections whose source
// address is NOT the announced peer: they must not be able to park the accept loop.
static bool RecvExactTimed(int socket, void *buffer, u32 size, int timeoutMs)
{
    u32 received = 0;
    const u64 start = svcGetSystemTick();
    while (received < size)
    {
        if (armTicksToNs(svcGetSystemTick() - start) > (u64)timeoutMs * 1000000ULL)
            return false;
        int res = SocketRecv(socket, (char *)buffer + received, size - received);
        if (res < 0)
            return false;
        received += (u32)res;
    }
    return true;
}

// Validate the connection ticket before replying to a peer behind a different NAT address.
static bool StreamHandshake(int socket, const u8 *requiredTicket, ProtoParsedHandshake *outParsed, bool *badProof)
{
    if (badProof)
        *badProof = false;
    if (!SocketSendControlAll(socket, PROTO_HANDSHAKE_HELLO, sizeof(PROTO_HANDSHAKE_HELLO)))
    {
        return false;
    }

    u8 reqBuffer[PROTO_HANDSHAKE_SIZE];
    const bool gotRequest = RecvExactTimed(socket, reqBuffer, sizeof(reqBuffer), 2000);
    if (!gotRequest)
    {
        return false;
    }

    if (requiredTicket)
    {
        u8 diff = 0;
        for (int i = 0; i < 6; i++)
            diff |= (u8)(reqBuffer[10 + i] ^ requiredTicket[i]);
        if (diff != 0)
        {
            if (badProof)
                *badProof = true;
            return false;
        }
    }

    // Accept_Any so a client may ALSO ask for audio (MetaFlags bit 1) on this same
    // connection; audio packets carry PacketMeta_Type_Audio in their header so the client can
    // demux. A client that asks for video only (the NRO) gets exactly the old behaviour.
    ProtoParsedHandshake parsed = ProtoHandshake(ProtoHandshakeAccept_Any, reqBuffer, sizeof(reqBuffer));
    if (parsed.Result.Code == Handshake_Ok && !parsed.RequestedVideo)
        parsed.Result.Code = Hanshake_InvalidChannel; // audio-only isn't supported on this channel
    // NXRP: only on the negotiated-feedback path, whose response fields are ours to use.
    parsed.UdpOffer = 0;
    if (parsed.Result.Code == Handshake_Ok && parsed.DeliveryFeedback && (reqBuffer[9] & UDPM_FEATURE_FLAG))
    {
        parsed.UdpOffer = UdpmVideo_Offer();
        if (parsed.UdpOffer)
        {
            // a LAN client sends straight to this console's address, so it must get the internal port.
            // The router's external port (9952 when another console holds 9951) only works from outside.
            {
                const u16 ext = UdpMapEnsure(UDPM_VIDEO_PORT);
                struct sockaddr_in peer = {0};
                socklen_t pl = sizeof(peer);
                const u32 ip =
                    bsdGetPeerName(socket, (struct sockaddr *)&peer, &pl) == 0 ? ntohl(peer.sin_addr.s_addr) : 0;
                const bool lan = (ip >> 24) == 10 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8;
                if (ext && ext != UDPM_VIDEO_PORT && !lan)
                    parsed.UdpOffer = (parsed.UdpOffer & ~(0xFFFFull << 32)) | ((u64)ext << 32);
            }
            parsed.Result.MemoryPools.ApplicationSize = parsed.UdpOffer;
            if (reqBuffer[9] & 0x40)
            {
                u8 key[16];
                UdpmVideo_NatOffer(key);
                memcpy(&parsed.Result.MemoryPools.ApplicationUsed, key, 8);
                memcpy(&parsed.Result.MemoryPools.AppletSize, key + 8, 8);
                parsed.Result.MemoryPools.AppletUsed = 0x3150554e;
            }
        }
    }
    if (!SocketSendControlAll(socket, &parsed.Result, sizeof(parsed.Result)))
    {
        if (parsed.UdpOffer)
            UdpmVideo_End();
        return false;
    }

    if (parsed.Result.Code != Handshake_Ok)
    {
        return false;
    }

    *outParsed = parsed;
    return true;
}

// Phase 2: stream video (and audio when asked) over a socket whose handshake StreamHandshake completed.
//  (multi-viewer): runs on the viewer's own thread; frames come from the capture hub's per-viewer queue
// instead of this thread reading the capture itself, so several of these loops can run at once.
static void StreamRun(Viewer *v, int socket, const ProtoParsedHandshake *parsedIn)
{
    const bool wantAudio = parsedIn->RequestedAudio;
    // NXRP: bind before attaching so no video queues up while we wait for the Hello.
    const bool udp = (parsedIn->UdpOffer) && UdpmVideo_Bind(socket, 2000, g_relayHost, g_relayPort);
    if (parsedIn->UdpOffer && !udp)
        UdpmVideo_End();
    Viewers_Attach(v, socket, wantAudio, parsedIn->AudioBatching,
                   parsedIn->OpusAudio);
    Viewers_EnableFeedback(v, parsedIn->DeliveryFeedback, udp);
    if ((!Viewers_VideoStartInput(v)))
    {
        Viewers_Detach(v);
        ProtoClientGlobalStateDisconnected();
        return;
    }
    u32 audioPacketsSent = 0;
    u64 audioBytesSent = 0;

    while (true)
    {
        if (udp)
        {
            UdpmVideo_Service();
            if (!UdpmVideo_Alive())
            {
                break;
            }
        }
        if (!((Viewers_VideoInputAlive(v))))
        {
            break;
        }

        if (wantAudio && !SendPendingAudio(v, &audioPacketsSent, &audioBytesSent, udp))
        {
            break;
        }

        if (Viewers_StreamStale())
        {
            break;
        }

        const u8 *wire = NULL;
        u32 wireSize = 0;
        const u64 waitBegin = svcGetSystemTick();
        const bool haveVideo = Viewers_NextVideo(v, &wire, &wireSize, 5);
        if (udp)
            UdpmVideo_RecordLoopTime(0, armTicksToNs(svcGetSystemTick() - waitBegin) / 1000);
        if (!haveVideo)
            continue;

        bool success = udp            ? Viewers_VideoSendUdp(v, wire, wireSize)
                       : (Viewers_VideoSend(v, wire, wireSize, true));
        if (!success)
        {
            break;
        }

        if (wantAudio && !SendPendingAudio(v, &audioPacketsSent, &audioBytesSent, udp))
        {
            break;
        }
    }

    Viewers_Detach(v); // also releases this viewer's pad / HOME / touch
    if (parsedIn->UdpOffer)
        UdpmVideo_End();
    ProtoClientGlobalStateDisconnected();
}

static void StreamVideoOverPunchedSocket(Viewer *v, int socket)
{
    ProtoParsedHandshake parsed;
    if (StreamHandshake(socket, NULL, &parsed, NULL))
        StreamRun(v, socket, &parsed);
}

// Picks a fresh local port for this one attempt, in the ephemeral-adjacent 20000-59999
// range, instead of always reusing the same fixed port from config. Doesn't change the
// fundamental requirement (registration and the punch itself must still share whatever
// port gets picked - that part is unchanged) - this only rules out any quirk a carrier's
// NAT might have tied to a specific, always-reused port number. svcGetSystemTick() is
// sufficient entropy for this - port selection isn't a security-sensitive choice.
static u16 PickRandomLocalPort(void)
{
    u64 tick = svcGetSystemTick();
    return (u16)(20000 + (tick % 40000));
}

void PunchProbe_SetRelay(const char *host, u16 port)
{
    // Port first: a torn read during an in-flight attempt at worst punches the old host once.
    g_relayPort = port;
    strncpy(g_relayHost, host, sizeof(g_relayHost) - 1);
    g_relayHost[sizeof(g_relayHost) - 1] = 0;
}

void PunchProbe_SetIdentity(const char *id, const char *password)
{
    // Password first, then id: PunchProbeAttempt treats an empty id as "no identity yet".
    strncpy(g_password, password, sizeof(g_password) - 1);
    g_password[sizeof(g_password) - 1] = 0;
    strncpy(g_id, id, sizeof(g_id) - 1);
    g_id[sizeof(g_id) - 1] = 0;
}

// ------------------------------------------------------------------------------------------------
// Router port mapping for the server punch (, hardware-proven by the NAT-PMP feasibility probe below).
// Hole punching fails when the phone's network rewrites source ports per destination (store Wi-Fi, CGNAT, many
// VPNs). So, in addition to the punch, the console asks the router (NAT-PMP, RFC 6886) to forward the SAME external
// port it registers with the server (the address the server announces to the phone) to a dedicated listener, and
// accepts a direct connection there. The phone already connects to that announced address, so neither the phone,
// the NRO nor the server needs to change.
// Safety: the mapping exists only while an attempt is running; the listener accepts ONLY a connection whose source
// IP is the peer IP the server announced (only someone who presented the Session ID/Password gets that), is closed
// the moment one is accepted or the attempt ends, and the mapping is deleted at the end (its lease is a backstop).
// Needs a router that speaks NAT-PMP; if it does not answer we remember that for a while and skip the delay.
// ------------------------------------------------------------------------------------------------
static bool NatPmpExchange(int udp, const struct sockaddr_in *gw, const u8 *req, int reqLen, u8 *resp, int respSize,
                           u8 expectOp, int *outLen);
static int LanListen(u16 port);

#define MAP_LEASE_S 3600
#define MAP_INTERNAL_PORT_A 49913
#define MAP_INTERNAL_PORT_B 49912
#define MAP_QUICK_ACCEPT_MS 3000
#define MAP_LATE_ACCEPT_MS 12000
#define NATPMP_RETRY_AFTER_S 600

typedef struct
{
    bool mapped;
    int udp;
    struct sockaddr_in gw;
    u16 internalPort;
    int listener;
} PunchMap;

static int g_natpmpSupport = 0; // 0 unknown, 1 the gateway answered NAT-PMP, -1 it did not
static u64 g_natpmpFailTick = 0;
static int g_natpmpAttempts = 3;

static void MapClose(PunchMap *m)
{
    if (m->listener != SOCKET_INVALID)
        SocketClose(&m->listener);
    if (m->mapped && m->udp != SOCKET_INVALID)
    {
        u8 del[12] = {0, 2, 0, 0, (u8)(m->internalPort >> 8), (u8)m->internalPort, 0, 0, 0, 0, 0, 0};
        u8 resp[32];
        int rl = 0;
        g_natpmpAttempts = 1;
        NatPmpExchange(m->udp, &m->gw, del, sizeof(del), resp, sizeof(resp), 130, &rl);
        g_natpmpAttempts = 3;
    }
    m->mapped = false;
    if (m->udp != SOCKET_INVALID)
        SocketClose(&m->udp);
}

static void MapOpen(PunchMap *m, u16 attemptPort, int slot)
{
    m->mapped = false;
    m->udp = SOCKET_INVALID;
    m->listener = SOCKET_INVALID;
    m->internalPort = 0;

    if (g_natpmpSupport < 0 &&
        svcGetSystemTick() - g_natpmpFailTick < armNsToTicks((u64)NATPMP_RETRY_AFTER_S * 1000000000ULL))
        return; // the gateway did not speak NAT-PMP recently: don't add a delay to every attempt

    u32 addr = 0, mask = 0, gw = 0, d1 = 0, d2 = 0;
    Result rc = nifmInitialize(NifmServiceType_User);
    if (R_SUCCEEDED(rc))
    {
        rc = nifmGetCurrentIpConfigInfo(&addr, &mask, &gw, &d1, &d2);
        nifmExit();
    }
    if (R_FAILED(rc) || gw == 0)
    {
        return;
    }

    memset(&m->gw, 0, sizeof(m->gw));
    m->gw.sin_family = AF_INET;
    m->gw.sin_port = htons(5351);
    m->gw.sin_addr.s_addr = gw;
    m->udp = SocketUdp();
    if (m->udp == SOCKET_INVALID || !SocketMakeNonBlocking(m->udp))
    {
        if (m->udp != SOCKET_INVALID)
            SocketClose(&m->udp);
        return;
    }

    // each viewer place has its own internal listener port, so attempts of different places never collide
    // on it (place 0 keeps the original pair of ports).
    const u16 baseA = slot == 0 ? MAP_INTERNAL_PORT_A : (u16)(49920 + slot * 2);
    const u16 baseB = slot == 0 ? MAP_INTERNAL_PORT_B : (u16)(49921 + slot * 2);
    const u16 internal = (attemptPort == baseA) ? baseB : baseA;
    u8 req[12] = {0,
                  2,
                  0,
                  0,
                  (u8)(internal >> 8),
                  (u8)internal,
                  (u8)(attemptPort >> 8),
                  (u8)attemptPort,
                  (u8)(MAP_LEASE_S >> 24),
                  (u8)(MAP_LEASE_S >> 16),
                  (u8)(MAP_LEASE_S >> 8),
                  (u8)MAP_LEASE_S};
    u8 resp[32];
    int rl = 0;
    g_natpmpAttempts = (g_natpmpSupport == 0) ? 1 : 3; // first ever check: one quick try, routers answer in ~25 ms
    const bool answered = NatPmpExchange(m->udp, &m->gw, req, sizeof(req), resp, sizeof(resp), 130, &rl) && rl >= 16;
    g_natpmpAttempts = 3;
    if (!answered)
    {
        g_natpmpSupport = -1;
        g_natpmpFailTick = svcGetSystemTick();
        SocketClose(&m->udp);
        return;
    }
    g_natpmpSupport = 1;

    const int result = (resp[2] << 8) | resp[3];
    const u16 ext = (u16)((resp[10] << 8) | resp[11]);
    if (result != 0)
    {
        SocketClose(&m->udp);
        return;
    }
    m->mapped = true;
    m->internalPort = internal;
    if (ext != attemptPort)
    {
        // The server announces the port it observed (= attemptPort), so a different external port is useless to the
        // phone.
        MapClose(m);
        return;
    }

    m->listener = LanListen(internal);
    if (m->listener == SOCKET_INVALID)
    {
        MapClose(m);
        return;
    }
}

// Accepts a connection on the mapped port. A connection from the peer IP the server announced is accepted at
// once (the old rule, unchanged). With a connection ticket (both ends got it from the relay) a connection from
// ANY other address is also accepted - but only after it presents the ticket in its handshake request; a
// connection that does not is dropped and the listener keeps waiting. On a ticket accept the handshake is
// already complete (hs->done), otherwise the caller runs the normal handshake.
// the TCP path works because the router forwards an inbound TCP port (NAT-PMP). Do the same for the UDPM video
// socket so the phone can reach it directly from any carrier NAT (only the phone initiates). Returns the external
// UDP port the router forwards to `internal`, or 0 (UDP punching only).
static u16 UdpMapEnsure(u16 internal)
{
    if (g_natpmpSupport < 0 &&
        svcGetSystemTick() - g_natpmpFailTick < armNsToTicks((u64)NATPMP_RETRY_AFTER_S * 1000000000ULL))
        return 0;
    u32 addr = 0, mask = 0, gw = 0, d1 = 0, d2 = 0;
    Result rc = nifmInitialize(NifmServiceType_User);
    if (R_SUCCEEDED(rc))
    {
        rc = nifmGetCurrentIpConfigInfo(&addr, &mask, &gw, &d1, &d2);
        nifmExit();
    }
    if (R_FAILED(rc) || gw == 0)
    {
        return 0;
    }
    struct sockaddr_in gwa;
    memset(&gwa, 0, sizeof(gwa));
    gwa.sin_family = AF_INET;
    gwa.sin_port = htons(5351);
    gwa.sin_addr.s_addr = gw;
    int udp = SocketUdp();
    if (udp == SOCKET_INVALID || !SocketMakeNonBlocking(udp))
    {
        if (udp != SOCKET_INVALID)
            SocketClose(&udp);
        return 0;
    }
    u8 req[12] = {0,
                  1,
                  0,
                  0,
                  (u8)(internal >> 8),
                  (u8)internal,
                  (u8)(internal >> 8),
                  (u8)internal,
                  (u8)(MAP_LEASE_S >> 24),
                  (u8)(MAP_LEASE_S >> 16),
                  (u8)(MAP_LEASE_S >> 8),
                  (u8)MAP_LEASE_S};
    u8 resp[32];
    int rl = 0;
    g_natpmpAttempts = (g_natpmpSupport == 0) ? 1 : 3;
    const bool answered = NatPmpExchange(udp, &gwa, req, sizeof(req), resp, sizeof(resp), 129, &rl) && rl >= 16;
    g_natpmpAttempts = 3;
    SocketClose(&udp);
    if (!answered)
    {
        return 0;
    }
    g_natpmpSupport = 1;
    const int result = (resp[2] << 8) | resp[3];
    const u16 ext = (u16)((resp[10] << 8) | resp[11]);
    if (result != 0)
    {
        return 0;
    }
    return ext;
}

typedef struct
{
    bool done;
    ProtoParsedHandshake parsed;
} PunchHandshakeState;

static int MapAccept(PunchMap *m, const char *peerHost, int timeoutMs, const u8 *ticket, PunchHandshakeState *hs)
{
    hs->done = false;
    if (m->listener == SOCKET_INVALID)
        return SOCKET_INVALID;
    const u32 want = inet_addr(peerHost);
    for (int waited = 0; waited < timeoutMs; waited += 100)
    {
        int conn = SOCKET_INVALID;
        struct sockaddr_in peer = {0};
        socklen_t pl = sizeof(peer);
        if (SocketTcpAccept(m->listener, &conn, (struct sockaddr *)&peer, &pl) == SocketAcceptError_OK)
        {
            if (peer.sin_addr.s_addr == want)
            {
                SocketClose(&m->listener); // nobody else gets in
                return conn;
            }
            if (ticket)
            {
                bool badProof = false;
                if (StreamHandshake(conn, ticket, &hs->parsed, &badProof))
                {
                    hs->done = true;
                    SocketClose(&m->listener); // nobody else gets in
                    return conn;
                }
            }
            SocketClose(&conn);
            continue; // there may be more already waiting
        }
        svcSleepThread(100ULL * 1000 * 1000);
    }
    return SOCKET_INVALID;
}

static void PunchProbeAttemptCore(Viewer *v, u16 attemptPort, PunchMap *map)
{

    int relaySocket = SocketTcpConnectFromPort(g_relayHost, g_relayPort, attemptPort);
    if (relaySocket == SOCKET_INVALID)
    {
        NetworkSupportLine("host registration: server connection failed");
        return;
    }

    char cmd[PROBE_LINE_MAX];
    char ticket[65];
    if (PunchWake_IsTemporarilySuspended())
    {
        SocketClose(&relaySocket);
        return;
    }
    bool protectedHost = PunchWake_DirectOwnershipEnabled();
    if (protectedHost && !PunchWake_GetDirectTicket(ticket, sizeof(ticket)))
    {
        NetworkSupportLine("host registration: no valid admission ticket");
        SocketClose(&relaySocket);
        return;
    }
    // the trailing 2 (PUNCHHOST2 / PUNCHHOSTAUTH2) = "I understand the connection ticket". An older
    // rendezvous server answers ERR BADCMD; register again the old way below (then the source-IP pin stays the
    // only rule). Provisioned (direct-owner) consoles register with the AUTH variant - they must get the ticket too.
    int n = protectedHost ? npf_snprintf(cmd, sizeof(cmd), "PUNCHHOSTAUTH2 %s %s\n", g_id, ticket)
                          : npf_snprintf(cmd, sizeof(cmd), "PUNCHHOST2 %s %s\n", g_id, g_password);
    memset(ticket, 0, sizeof(ticket));
    if (n <= 0 || (size_t)n >= sizeof(cmd) || !SocketSendControlAll(relaySocket, cmd, (u32)n))
    {
        NetworkSupportLine("host registration: request send failed");
        SocketClose(&relaySocket);
        return;
    }

    char resp[PROBE_LINE_MAX];
    bool ticketAware = true;
    resp[0] = 0;
    if (ReadLine(relaySocket, resp, sizeof(resp), 5000) && strcmp(resp, "ERR BADCMD") == 0)
    {
        ticketAware = false;
        SocketClose(&relaySocket);
        relaySocket = SocketTcpConnectFromPort(g_relayHost, g_relayPort, attemptPort);
        // The old verb is the new one without its trailing '2' (PUNCHHOST2 -> PUNCHHOST, PUNCHHOSTAUTH2 ->
        // PUNCHHOSTAUTH).
        char *verbEnd = strchr(cmd, ' ');
        if (verbEnd && verbEnd > cmd && verbEnd[-1] == '2')
        {
            memmove(verbEnd - 1, verbEnd, (size_t)(n - (int)(verbEnd - cmd)) + 1); // includes the NUL
            n--;
        }
        if (relaySocket == SOCKET_INVALID || n <= 0 || (size_t)n >= sizeof(cmd) ||
            !SocketSendControlAll(relaySocket, cmd, (u32)n))
        {
            if (relaySocket != SOCKET_INVALID)
                SocketClose(&relaySocket);
            return;
        }
        resp[0] = 0;
        ReadLine(relaySocket, resp, sizeof(resp), 5000);
    }
    if (strcmp(resp, "OK") != 0)
    {
        const char *reason = "server rejected request";
        if (!resp[0])
            reason = "no reply";
        else if (strcmp(resp, "ERR OWNERAUTH") == 0)
            reason = "owner authentication rejected";
        NetworkSupportLine("host registration: %s", reason);
        SocketClose(&relaySocket);
        return;
    }

    NetworkSupportLine("host registration accepted; waiting for join");
    memset(cmd, 0, sizeof(cmd)); // it may hold the one-time direct ticket

    char peerLine[PROBE_LINE_MAX];
    bool gotPeer = false;
    bool cancelled = false;
    int waitedMs = 0;
    // measure REAL elapsed time. This loop used to add 200 per pass, but ReadLine's "200 ms" is 200 one-ms
    // recv polls whose real cost is ~0.5-1 s each, so the "50 s" wait actually ran ~125 s (hardware-observed:
    // registered 17:47:47, timed out 17:49:52) - keeping the console busy and the router port open far longer than
    // intended.
    const u64 waitStartTick = svcGetSystemTick();
    atomic_store(&g_cancelPunchWait, false);
    atomic_store(&g_punchWaiting, true);
    while (waitedMs < PUNCH_PROBE_WAIT_MS)
    {
        if (atomic_load(&g_cancelPunchWait))
        {
            cancelled = true;
            break;
        }
        if (ReadLine(relaySocket, peerLine, sizeof(peerLine), PUNCH_PROBE_WAIT_CHECK_MS))
        {
            gotPeer = true;
            break;
        }
        waitedMs = (int)(armTicksToNs(svcGetSystemTick() - waitStartTick) / 1000000ULL);
    }
    atomic_store(&g_punchWaiting, false);
    // the register/wait phase is over: another punch attempt (for another viewer) may start now
    Viewers_PunchPhaseDone(v);

    SocketClose(&relaySocket);

    if (cancelled)
    {
        return;
    }
    if (!gotPeer)
    {
        NetworkSupportLine("host registration: no join received in %d ms", waitedMs);
        return;
    }

    char peerHost[64];
    u16 peerPort;
    NetworkSupportLine("join notification received");
    if (!ParsePeerLine(peerLine, peerHost, sizeof(peerHost), &peerPort))
    {
        return;
    }

    // Optional connection ticket: "PEER <ip:port> <12 hex chars>" (only when we registered with PUNCHHOST2
    // and the connector used PUNCHJOIN2). Never logged.
    u8 connTicket[6] = {0};
    bool haveTicket = false;
    if (ticketAware)
    {
        const char *sp = strchr(peerLine + 5, ' ');
        if (sp && strlen(sp + 1) == 12)
        {
            haveTicket = true;
            for (int i = 0; i < 12; i++)
            {
                const char c = sp[1 + i];
                int v = (c >= '0' && c <= '9')   ? c - '0'
                        : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                        : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                                                 : -1;
                if (v < 0)
                {
                    haveTicket = false;
                    break;
                }
                if (i % 2 == 0)
                    connTicket[i / 2] = (u8)(v << 4);
                else
                    connTicket[i / 2] |= (u8)v;
            }
            if (!haveTicket)
                memset(connTicket, 0, sizeof(connTicket));
        }
    }

    // 1) The phone may already be connecting to the mapped port (it connects to the announced address right away).
    if (map->listener != SOCKET_INVALID)
    {
        PunchHandshakeState hs;
        int conn = MapAccept(map, peerHost, MAP_QUICK_ACCEPT_MS, haveTicket ? connTicket : NULL, &hs);
        if (conn != SOCKET_INVALID)
        {
            if (hs.done)
                StreamRun(v, conn, &hs.parsed);
            else
                StreamVideoOverPunchedSocket(v, conn);
            SocketClose(&conn);
            return;
        }
    }

    // 2) Classic hole punch (the listener, if any, stays open meanwhile - it uses a different local port).

    int punchSocket = SocketTcpConnectFromPort(peerHost, peerPort, attemptPort);
    if (punchSocket == SOCKET_INVALID)
    {
        // 3) The punch failed (symmetric NAT etc.): give the mapped port a little longer - the phone keeps trying for
        // ~15 s.
        PunchHandshakeState hs;
        int conn = MapAccept(map, peerHost, MAP_LATE_ACCEPT_MS, haveTicket ? connTicket : NULL, &hs);
        if (conn != SOCKET_INVALID)
        {
            if (hs.done)
                StreamRun(v, conn, &hs.parsed);
            else
                StreamVideoOverPunchedSocket(v, conn);
            SocketClose(&conn);
        }
        return;
    }

    SocketClose(&map->listener); // the punch won: no need to keep the mapped port open
    StreamVideoOverPunchedSocket(v, punchSocket);
    SocketClose(&punchSocket);
}

static void PunchProbeAttempt(Viewer *v)
{
    if (g_id[0] == 0)
    {
        return;
    }

    NetworkSupportLine("connection attempt started");
    const u16 attemptPort = PickRandomLocalPort();
    PunchMap map;
    MapOpen(&map, attemptPort,
            Viewers_Slot(v)); // asks the router for external attemptPort -> a local listener (skipped if unsupported)
    NetworkSupportLine("router setup finished: mapped=%d; connecting to server", map.mapped);
    PunchProbeAttemptCore(v, attemptPort, &map);
    MapClose(&map); // listener closed, mapping deleted, on every exit path
}

static void PunchProbeThread(void *arg)
{
    Viewer *v = (Viewer *)arg;
    PunchProbeAttempt(v);
    Viewers_Release(v);
    NetworkSupportLine("connection attempt released its viewer slot");
}

// Starts the viewer's own session thread (its stack is part of the viewer place). The previous session on this place
// has ended (it released the place as its last act), so joining it is immediate.
static void LaunchViewerThread(Viewer *v, ThreadFunc fn)
{
    if (Viewers_ThreadStarted(v))
        JoinThread(Viewers_Thread(v));
    Viewers_MarkThreadStarted(v);
    LaunchThread(Viewers_Thread(v), fn, v, Viewers_StackBase(v), Viewers_StackSize(), 0x2C);
}

static void FirePunchProbe(void)
{
    if (PunchWake_IsTemporarilySuspended())
        return;
    Viewer *v = Viewers_Reserve(ViewerKind_Punch);
    if (v == NULL)
    {
        NetworkSupportLine("wake deferred: no free viewer slot or another attempt pending");
        return;
    }
    NetworkSupportLine("viewer slot %d reserved; starting connection worker", Viewers_Slot(v));
    LaunchViewerThread(v, PunchProbeThread);
}

// Entry point for PunchWake.c's UDP CHECKIN loop - fires the exact same probe attempt as a
// manual Y-hold, just from a different trigger source. Deliberately skips the button
// watcher's Capture_IsRealVideoRecentlyActive guard: that guard exists only to avoid
// colliding with the physical Y button's native hold-to-capture gesture, which doesn't
// apply to a network-originated wake.
void PunchProbe_RequestWake(void)
{
    if (PunchWake_IsTemporarilySuspended())
        return;
    if (Viewers_Count() >= Viewers_Max())
    {
        NetworkSupportLine("wake deferred: all viewer slots occupied");
        return;
    }
    if (Viewers_PunchWaiting())
    {
        NetworkSupportLine("wake deferred: connection already waiting for join");
        return;
    }
    FirePunchProbe();
}

// Lets PunchWake.c's UDP check-in loop reuse the same relay/id/password this file already
// loaded from server.txt, instead of re-reading/duplicating that config. Returns false
// if PunchProbe_StartButtonWatcher hasn't successfully loaded a config yet.
bool PunchProbe_GetRelayConfig(char *outHost, size_t hostSize, u16 *outPort, char *outId, size_t idSize,
                               char *outPassword, size_t passwordSize)
{
    if (g_relayHost[0] == 0 || g_relayPort == 0)
        return false;
    strncpy(outHost, g_relayHost, hostSize - 1);
    outHost[hostSize - 1] = 0;
    *outPort = g_relayPort;
    strncpy(outId, g_id, idSize - 1);
    outId[idSize - 1] = 0;
    strncpy(outPassword, g_password, passwordSize - 1);
    outPassword[passwordSize - 1] = 0;
    return true;
}

// ---------------------------------------------------------------------------------------------
// LAN discovery : lets a client on the SAME network find and join this console with
// no Rendezvous server and no internet - and fixes the "both behind one router" case, where the
// internet-facing punch can't work. Same Session ID + Password as the server path.
//
//   client -> UDP broadcast :9913   "NXLAN1 DISCOVER <id>"
//   host   -> UDP unicast reply     "NXLAN1 HERE <id> 9914"   (only if <id> is ours and we're idle;
//                                                              a one-shot TCP listener on :9914 is
//                                                              opened for ~8 s)
//   client -> TCP connect host:9914
//   host   -> "NXLAN1 CHALLENGE <16 hex nonce>\n"
//   client -> "NXLAN1 AUTH <16 hex client nonce> <sha256 hex>\n"
//             where sha256 = SHA-256( password ":" hostNonce ":" clientNonce ) - the password itself
//             never crosses the wire
//   host   -> "NXLAN1 OK\n" (or "NXLAN1 DENIED\n" + close), then the normal NxRemotePlay video handshake
//             and stream (StreamVideoOverPunchedSocket) run on that socket.
// Takes a viewer place for the whole session (see Viewers.h); LAN and server-punch viewers share the same places.
// Socket budget (bsd session cap 9): one persistent UDP listener + a transient TCP listener that
// is closed the instant the client is accepted.
// ---------------------------------------------------------------------------------------------
#define LAN_UDP_PORT 9913
#define LAN_TCP_PORT 9914
#define LAN_SOCK_MAX_AGE_S 60 // rebuild the idle discovery socket at least this often

static void LanToHex(const u8 *in, size_t n, char *out)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++)
    {
        out[i * 2] = hex[in[i] >> 4];
        out[i * 2 + 1] = hex[in[i] & 0xF];
    }
    out[n * 2] = 0;
}

// Not a security-grade RNG (no csrng service in this sysmodule) - a per-session nonce only has
// to be unpredictable enough that a captured AUTH can't be replayed to a later session.
static void LanNonce(char out[17])
{
    static u64 counter = 0x9E3779B97F4A7C15ull;
    counter = counter * 6364136223846793005ull + svcGetSystemTick() + 1442695040888963407ull;
    u64 v = counter ^ (counter >> 29);
    u8 bytes[8];
    for (int i = 0; i < 8; i++)
    {
        bytes[i] = (u8)(v & 0xFF);
        v >>= 8;
    }
    LanToHex(bytes, 8, out);
}

// One-shot TCP listener that reports failure instead of retrying forever (unlike SocketTcpListen).
static int LanListen(u16 port)
{
    int s = bsdSocket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return SOCKET_INVALID;
    const int one = 1;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (!SocketMakeNonBlocking(s) || bsdSetSockOpt(s, SOL_SOCKET, SO_REUSEADDR, (void *)&one, sizeof(one)) == -1 ||
        bsdBind(s, (struct sockaddr *)&addr, sizeof(addr)) == -1 || bsdListen(s, 1) == -1)
    {
        bsdClose(s);
        return SOCKET_INVALID;
    }
    return s;
}

// Challenge/response, then hands the authenticated socket to the normal NxRemotePlay stream.
static void LanAuthAndStream(Viewer *v, int conn)
{
    char hostNonce[17];
    LanNonce(hostNonce);
    char msg[64];
    int n = npf_snprintf(msg, sizeof(msg), "NXLAN1 CHALLENGE %s\n", hostNonce);
    if (n <= 0 || !SocketSendControlAll(conn, msg, (u32)n))
        return;

    char line[160];
    if (!ReadLine(conn, line, sizeof(line), 5000))
    {
        return;
    }

    // "NXLAN1 AUTH <clientNonce:16> <hash:64>"
    char clientNonce[17] = {0}, hash[65] = {0};
    if (strncmp(line, "NXLAN1 AUTH ", 12) != 0 || strlen(line) != 12 + 16 + 1 + 64 || line[12 + 16] != ' ')
    {
        SocketSendControlAll(conn, "NXLAN1 DENIED\n", 14);
        return;
    }
    memcpy(clientNonce, line + 12, 16);
    memcpy(hash, line + 12 + 17, 64);

    char material[128];
    int mlen = npf_snprintf(material, sizeof(material), "%s:%s:%s", g_password, hostNonce, clientNonce);
    u8 digest[32];
    sha256CalculateHash(digest, material, (size_t)mlen);
    char expected[65];
    LanToHex(digest, 32, expected);

    // Constant-time compare so timing can't leak how much of a guess matched.
    u8 diff = 0;
    for (int i = 0; i < 64; i++)
        diff |= (u8)(expected[i] ^ hash[i]);
    if (diff != 0)
    {
        SocketSendControlAll(conn, "NXLAN1 DENIED\n", 14);
        return;
    }

    if (!SocketSendControlAll(conn, "NXLAN1 OK\n", 10))
        return;
    StreamVideoOverPunchedSocket(v, conn);
}

static int g_lanConn[VIEWER_MAX];

static void LaunchViewerThread(Viewer *v, ThreadFunc fn);

static void LanSessionThread(void *arg)
{
    Viewer *v = (Viewer *)arg;
    int conn = g_lanConn[Viewers_Slot(v)];
    LanAuthAndStream(v, conn);
    SocketClose(&conn);
    Viewers_Release(v);
}

static void LanHostThread(void *arg)
{
    (void)arg;
    int udp = SOCKET_INVALID;
    // the discovery socket used to be opened once and never again. After the console
    // sleeps / the WiFi reconnects a bsd UDP socket goes deaf while recvfrom just keeps returning
    // EAGAIN (indistinguishable from "no packet"), so LAN discovery silently died until reboot
    // while the server path (fresh sockets every cycle) kept working. Rebuild it when the loop was
    // stalled (sleep), the IP changed, or every LAN_SOCK_MAX_AGE_S regardless, and log why.
    const u64 tickHz = 19200000ULL;
    u64 lastTick = svcGetSystemTick();
    u64 sockBornTick = lastTick;
    u64 lastBeatTick = lastTick;
    long curIP = 0;

    while (true)
    {
        u64 nowTick = svcGetSystemTick();
        u64 gapMs = (nowTick - lastTick) * 1000 / tickHz;
        lastTick = nowTick;

        if (udp != SOCKET_INVALID)
        {
            const char *why = NULL;
            long ip = gethostid();
            if (gapMs > 3000)
                why = "loop stalled (console slept?)";
            else if (ip != curIP && (nowTick - sockBornTick) > 2 * tickHz)
                why = "IP changed";
            else if ((nowTick - sockBornTick) > (u64)LAN_SOCK_MAX_AGE_S * tickHz)
                why = "periodic refresh";
            if (why)
            {
                bsdClose(udp);
                udp = SOCKET_INVALID;
            }
        }

        if (udp == SOCKET_INVALID)
        {
            udp = bsdSocket(AF_INET, SOCK_DGRAM, 0);
            struct sockaddr_in addr = {0};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port = htons(LAN_UDP_PORT);
            const bool sockOk =
                udp >= 0 && SocketMakeNonBlocking(udp) && bsdBind(udp, (struct sockaddr *)&addr, sizeof(addr)) != -1;
            if (!sockOk)
            {
                if (udp >= 0)
                    bsdClose(udp);
                udp = SOCKET_INVALID;
                svcSleepThread(10ULL * 1000 * 1000 * 1000);
                lastTick = svcGetSystemTick(); // our own 10s sleep is not a "console slept" gap
                continue;
            }
            sockBornTick = svcGetSystemTick();
            curIP = gethostid();
        }

        // Once a minute: prove the thread is alive and show what it has seen (diagnostic).
        if ((nowTick - lastBeatTick) > 60 * tickHz)
        {
            lastBeatTick = nowTick;
        }

        char buf[96];
        struct sockaddr_in from = {0};
        socklen_t fromLen = sizeof(from);
        ssize_t r = bsdRecvFrom(udp, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fromLen);
        if (r <= 0)
        {
            // -1/EAGAIN is the normal idle case; anything else means the socket is bad - drop it
            // so the next pass builds a fresh one.
            if (r < 0 && SocketNativeErrno() != 11)
            {
                bsdClose(udp);
                udp = SOCKET_INVALID;
            }
            svcSleepThread(100ULL * 1000 * 1000);
            continue;
        }
        buf[r] = 0;
        while (r > 0 && (buf[r - 1] == '\n' || buf[r - 1] == '\r'))
            buf[--r] = 0;

        // "NXLAN1 DISCOVER <id>" - only answer for OUR id, while hosting is on (identity set)
        // and nothing else (a server punch / another LAN session) is in progress.
        if (strncmp(buf, "NXLAN1 DISCOVER ", 16) != 0)
            continue;
        if (PunchWake_IsTemporarilySuspended())
            continue;
        const char *id = buf + 16;
        if (g_id[0] == 0 || strcmp(id, g_id) != 0)
        {
            // Diagnostic : a DISCOVER that arrives but is ignored used to be invisible.
            continue;
        }
        //  (multi-viewer): a LAN client takes a viewer place instead of the single "probe in progress" flag.
        Viewer *viewer = Viewers_Reserve(ViewerKind_Lan);
        if (!viewer && Viewers_Count() >= Viewers_Max() && atomic_load(&g_punchWaiting))
        {
            // every place is taken and one of them is only a server punch still WAITING for a connector
            // that may never come. A LAN client is here right now, so cancel that wait and take over (up to ~2.5 s for
            // it to unwind).
            atomic_store(&g_cancelPunchWait, true);
            for (int i = 0; i < 25 && !viewer; i++)
            {
                svcSleepThread(100ULL * 1000 * 1000);
                viewer = Viewers_Reserve(ViewerKind_Lan);
            }
        }
        if (!viewer)
        {
            // Every viewer place is in use (LAN or server punch): say so, so the client can report "busy" instead of
            // searching/waiting for nothing. Never reveals anything but the id the client already asked about.
            char busy[48];
            int bn = npf_snprintf(busy, sizeof(busy), "NXLAN1 BUSY %s", g_id);
            SocketUDPSendTo(udp, busy, (u32)bn, (struct sockaddr *)&from, sizeof(from));
            continue;
        }

        int listener = LanListen(LAN_TCP_PORT);
        if (listener == SOCKET_INVALID)
        {
            Viewers_Release(viewer);
            continue;
        }

        char reply[48];
        int rn = npf_snprintf(reply, sizeof(reply), "NXLAN1 HERE %s %d", g_id, LAN_TCP_PORT);
        SocketUDPSendTo(udp, reply, (u32)rn, (struct sockaddr *)&from, sizeof(from));

        int conn = SOCKET_INVALID;
        struct sockaddr_in peer = {0};
        for (int i = 0; i < 80 && conn == SOCKET_INVALID; i++) // ~8 s
        {
            socklen_t peerLen = sizeof(peer);
            if (SocketTcpAccept(listener, &conn, (struct sockaddr *)&peer, &peerLen) == SocketAcceptError_OK)
                break;
            conn = SOCKET_INVALID;
            svcSleepThread(100ULL * 1000 * 1000);
        }
        SocketClose(&listener); // release the socket right away (budget)

        if (conn == SOCKET_INVALID)
        {
            Viewers_Release(viewer);
        }
        else if (peer.sin_addr.s_addr != from.sin_addr.s_addr)
        {
            SocketClose(&conn);
            Viewers_Release(viewer);
        }
        else
        {
            // The session (auth, handshake, streaming) runs on the viewer's own thread so this loop can keep answering
            // DISCOVER for the next viewer.
            g_lanConn[Viewers_Slot(viewer)] = conn;
            LaunchViewerThread(viewer, LanSessionThread);
        }
        lastTick = svcGetSystemTick(); // a long LAN session is not a "console slept" stall
    }
}

static u8 alignas(0x1000) LanHostStackArea[0x6000 + LOGGING_STACK_BOOST];
static Thread LanHostThreadHandle;

static u8 alignas(0x1000) ButtonWatcherStackArea[0x1000 + LOGGING_STACK_BOOST];
static Thread ButtonWatcherThreadHandle;

static void ButtonWatcherThread(void *arg)
{
    (void)arg;

    PadState pad;
    padInitializeDefault(&pad);

    bool wasHeld = false;
    u64 holdStartTick = 0;
    bool triggeredThisHold = false;

    while (true)
    {
        svcSleepThread(100ULL * 1000 * 1000); // 100ms poll

        padUpdate(&pad);
        bool isHeld = (padGetButtons(&pad) & HidNpadButton_Y) != 0;

        if (isHeld && !wasHeld)
        {
            holdStartTick = svcGetSystemTick();
            triggeredThisHold = false;
        }

        if (isHeld && !triggeredThisHold && armTicksToNs(svcGetSystemTick() - holdStartTick) >= TRIGGER_HOLD_NS)
        {
            triggeredThisHold = true;

            if (!Capture_IsRealVideoRecentlyActive(GAME_FOCUSED_WINDOW_NS) && Viewers_Count() < Viewers_Max())
                FirePunchProbe();
        }

        wasHeld = isHeld;
    }
}

// One request/response with the gateway, RFC 6886's 250/500/1000 ms retry schedule.
static bool NatPmpExchange(int udp, const struct sockaddr_in *gw, const u8 *req, int reqLen, u8 *resp, int respSize,
                           u8 expectOp, int *outLen)
{
    int windowMs = 250;
    for (int attempt = 0; attempt < g_natpmpAttempts; attempt++)
    {
        if (!SocketUDPSendTo(udp, req, (u32)reqLen, (struct sockaddr *)gw, sizeof(*gw)))
        {
            return false;
        }
        for (int waited = 0; waited < windowMs; waited += 25)
        {
            struct sockaddr_in from = {0};
            socklen_t fl = sizeof(from);
            ssize_t r = bsdRecvFrom(udp, resp, (size_t)respSize, 0, (struct sockaddr *)&from, &fl);
            if (r >= 8 && resp[0] == 0 && resp[1] == expectOp)
            {
                *outLen = (int)r;
                return true;
            }
            svcSleepThread(25ULL * 1000 * 1000);
        }
        windowMs *= 2;
    }
    return false;
}

void PunchProbe_StartButtonWatcher(FsFileSystem *sdCard)
{
    if (!ReadConfig(sdCard))
    {
        return;
    }

    Viewers_Init();

    LaunchThread(&LanHostThreadHandle, LanHostThread, NULL, LanHostStackArea, sizeof(LanHostStackArea), 0x2C);

    LaunchThread(&ButtonWatcherThreadHandle, ButtonWatcherThread, NULL, ButtonWatcherStackArea,
                 sizeof(ButtonWatcherStackArea), 0x2C);
}

