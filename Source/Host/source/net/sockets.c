#include "../core.h"

#include <string.h>
#include <fcntl.h>       // fcntl
#include <netinet/tcp.h> // IPPROTO_TCP, TCP_NODELAY

#include "sockets.h"
#define NX_EAGAIN 11
#define NX_O_NONBLOCK 0x800
// was 119 (misremembered from an unrelated codebase/context) - real hardware
// test showed bsdConnect actually returning errno=115 for EINPROGRESS here, which matches
// standard Linux/POSIX numbering (same family NX_EAGAIN=11 above already matches). Trust
// the observed value over a recalled one if this class of mismatch ever recurs.
#define NX_EINPROGRESS 115

#include "../modes/modes.h"
#include "../capture.h"

// Forward-declared: real definition (and the PollResult enum) is further down this file,
// but SocketTcpConnectFromPort (defined earlier) needs it for its own bounded-connect fix.
typedef enum
{
    PollResult_Timeout,
    PollResult_Disconnected,
    PollResult_CanWrite,
    PollResult_CanRead,
    PollResult_Other
} PollResult;
static PollResult PollSocket(int socket, int timeoutMs);

// We want to statically allocate all socketing resources to avoid heap fragmentation
// This means we use the bsd service API directly instead of the libnx wrapper

static bool SocketReady;
static u32 BsdSessions;

u32 SocketBsdSessions(void)
{
    return BsdSessions;
}
static bool HasNifm;

#define PAGE_ALIGN(x) ((x + 0xFFF) &~ 0xFFF)

#define NON_ZERO(x, y) (x == 0 ? y : x)

// This is our biggest offender in memory usage but lower values may cause random hanging over long streaming sessions.
// They can be workarounded with auto reconnection and non-blocking sockets but i prefer keeping the connection stable
// Having enough capacity ensures that we can handle any packet size without dropping frames.
#define TCP_TX_SZ (16 * 1024)
#define TCP_RX_SZ (8 * 1024)

#define TCP_TX_MAX_SZ (200 * 1024)
#define TCP_RX_MAX_SZ 0

// NXRP: 32 KiB so one paced UDPM burst (16 KiB) never hits EAGAIN.
#define UDP_TX_SZ (32 * 1024)
#define UDP_RX_SZ (4 * 1024)

#define SOCK_EFFICIENCY 2

// Formula taken from libnx itslf
#define TMEM_SIZE PAGE_ALIGN(NON_ZERO(TCP_TX_MAX_SZ, TCP_TX_SZ) + NON_ZERO(TCP_RX_MAX_SZ, TCP_RX_SZ) + UDP_TX_SZ + UDP_RX_SZ) * SOCK_EFFICIENCY

static u8 alignas(0x1000) TmemBackingBuffer[TMEM_SIZE];

void SocketInit()
{
    if (SocketReady)
        return;

    // RelayMode.c's video/audio/hidplus threads all start dialing the relay almost
    // immediately after this function returns (hidplus's own thread only adds a 2s
    // stagger - nothing waits for the network to actually be routable first). A
    // non-blocking-connect rewrite of SocketTcpConnect (see its own comment) failed
    // EVERY attempt with ENETDOWN ("network interface is not configured"), even
    // though the network was reachable moments later - consistent with the WiFi
    // interface genuinely not being fully up yet at the exact instant these threads
    // first try to connect, with a *blocking* connect() apparently tolerating that
    // window (its single IPC call can just sit there until the route becomes valid)
    // in a way a non-blocking one-shot check can't. Block here on nifm reporting a
    // real Connected status before touching bsd at all - bounded to ~10s so a boot
    // with no network at all doesn't hang forever. This mirrors a pattern already
    // proven necessary for UDP debug logging in the sibling NxRemotePlay-Combined repo's
    // sockets.c; applied here unconditionally (not just under UDP_LOGGING) since
    // relay connectivity itself, not just logging, depends on it in this fork.
    Result nifmRc = nifmInitialize(NifmServiceType_User);
    if (R_SUCCEEDED(nifmRc))
    {
        HasNifm = true;
        for (int i = 0; i < 40; i++)
        {
            NifmInternetConnectionType connType;
            u32 wifiStrength;
            NifmInternetConnectionStatus connStatus = NifmInternetConnectionStatus_ConnectingUnknown1;
            Result rc = nifmGetInternetConnectionStatus(&connType, &wifiStrength, &connStatus);
            if (R_SUCCEEDED(rc) && connStatus == NifmInternetConnectionStatus_Connected)
            {
                break;
            }
            svcSleepThread(250ULL * 1000 * 1000); // 250ms
        }
    }
    else
    {
    }

    memset(TmemBackingBuffer, 0, sizeof(TmemBackingBuffer));

    const BsdInitConfig config = {.version = 1,
                                  .tmem_buffer = TmemBackingBuffer,
                                  .tmem_buffer_size = TMEM_SIZE,

                                  .tcp_tx_buf_size = TCP_TX_SZ,
                                  .tcp_rx_buf_size = TCP_RX_SZ,
                                  .tcp_tx_buf_max_size = TCP_TX_MAX_SZ,
                                  .tcp_rx_buf_max_size = TCP_RX_MAX_SZ,

                                  .udp_tx_buf_size = UDP_TX_SZ,
                                  .udp_rx_buf_size = UDP_RX_SZ,

                                  .sb_efficiency = SOCK_EFFICIENCY};

    // Was 3, sized for stock NxRemotePlay's own sockets (TCP video, TCP audio, UDP broadcast).
    // Bumped to 6 for 2 more: our own UDP debug logger and sys-hidplus's UDP socket (port
    // 8000). That count went stale the moment RelayMode.c's hidplus relay tunnel added a
    // *third* persistent TCP connection (Relay_HidPlusBridgeThread) on top of the existing
    // video/audio ones - 6 was needed just for steady state with zero spare, so any
    // transient extra socket (a retry's old socket not yet fully torn down, nifm's own
    // internal usage, etc.) pushed the bsd session over its cap. Once that happens every
    // subsequent bsdSocket() call fails silently, not just new relay connections - this
    // was diagnosed as the cause of a session where *nothing* came through the UDP debug
    // logger at all (not merely "relay won't register"), which pointed at exhaustion of
    // the whole session rather than a relay-specific bug. Bumped to 9 (6 potential
    // simultaneous consumers - 3 relay TCP + broadcast + 2 UDP - plus 3 spare, not 1) so
    // there's real headroom this time, not another single-count bump destined to go stale
    // at the next added socket consumer.
    //
    // Sixteen sessions did not reduce the send stalls and leave less of this shared service for other programs.
    const u32 sessionCount = 9;
    Result bsdRc = bsdInitialize(&config, sessionCount, BsdServiceType_User);
    if (R_SUCCEEDED(bsdRc))
    {
        BsdSessions = sessionCount;
    }
    R_THROW(bsdRc);

    SocketReady = true;
}

void SocketDeinit()
{
    if (!SocketReady)
        return;

    if (HasNifm)
    {
        nifmExit();
        HasNifm = false;
    }

    bsdExit();

    SocketReady = false;
}

void SocketClose(int *socket)
{
    if (*socket != SOCKET_INVALID)
    {
        bsdClose(*socket);
        *socket = SOCKET_INVALID;
    }
}

int SocketUdp()
{
    return bsdSocket(AF_INET, SOCK_DGRAM, 0);
}

// recreate the UDP debug-log socket. After the console sleeps or the WiFi reconnects
// the old bsd socket silently stops delivering (sends "succeed", nothing arrives) and nothing
// recreated it except the TCP-accept NetDown hack - so all diagnostics went dark until reboot.

// hostname support (default server is nxremoteplay.com). The sysmodule uses raw bsd
// sockets (no sfdnsres), so resolve with a minimal DNS A-record query over UDP, trying the
// console's own DNS servers (nifm) then 1.1.1.1 / 8.8.8.8. Results are cached for 5 minutes;
// a stale cached address is reused if a refresh fails. Literal IPv4 addresses bypass all of this.
// Blocking, bounded (<= ~1s per server); call only from background threads. Returns the address
// in network byte order, or INADDR_NONE.
u32 SocketResolveHostV4(const char *host)
{
    if (!host || !host[0])
        return INADDR_NONE;
    u32 literal = inet_addr(host);
    if (literal != INADDR_NONE)
        return literal;

    static char cacheName[64];
    static u32 cacheAddr = INADDR_NONE;
    static u64 cacheTick;
    const u64 now = armGetSystemTick();
    if (cacheAddr != INADDR_NONE && strcmp(cacheName, host) == 0 && (now - cacheTick) < armGetSystemTickFreq() * 300ULL)
        return cacheAddr;

    u32 servers[4];
    int nServers = 0;
    if (HasNifm)
    {
        u32 a, m, g, d1 = 0, d2 = 0;
        if (R_SUCCEEDED(nifmGetCurrentIpConfigInfo(&a, &m, &g, &d1, &d2)))
        {
            if (d1)
                servers[nServers++] = d1;
            if (d2)
                servers[nServers++] = d2;
        }
    }
    servers[nServers++] = inet_addr("1.1.1.1");
    servers[nServers++] = inet_addr("8.8.8.8");

    u8 query[300];
    size_t qlen = 0;
    const u16 txid = (u16)(now & 0xFFFF);
    query[qlen++] = (u8)(txid >> 8);
    query[qlen++] = (u8)txid;
    query[qlen++] = 0x01;
    query[qlen++] = 0x00; // recursion desired
    query[qlen++] = 0;
    query[qlen++] = 1; // QDCOUNT
    for (int i = 0; i < 6; i++)
        query[qlen++] = 0; // AN/NS/AR counts
    const char *label = host;
    while (*label)
    {
        const char *dot = strchr(label, '.');
        size_t len = dot ? (size_t)(dot - label) : strlen(label);
        if (len == 0 || len > 63 || qlen + len + 6 > sizeof(query))
            return INADDR_NONE;
        query[qlen++] = (u8)len;
        memcpy(query + qlen, label, len);
        qlen += len;
        label += len + (dot ? 1 : 0);
    }
    query[qlen++] = 0;
    query[qlen++] = 0;
    query[qlen++] = 1; // QTYPE A
    query[qlen++] = 0;
    query[qlen++] = 1; // QCLASS IN

    u32 result = INADDR_NONE;
    for (int si = 0; si < nServers && result == INADDR_NONE; si++)
    {
        int sock = bsdSocket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0)
            continue;
        SocketMakeNonBlocking(sock);
        struct sockaddr_in dst = {0};
        dst.sin_family = AF_INET;
        dst.sin_port = htons(53);
        dst.sin_addr.s_addr = servers[si];
        if (bsdSendTo(sock, query, qlen, 0, (struct sockaddr *)&dst, sizeof(dst)) == (ssize_t)qlen)
        {
            for (int waited = 0; waited < 1000 && result == INADDR_NONE; waited += 25)
            {
                u8 resp[512];
                ssize_t n = bsdRecvFrom(sock, resp, sizeof(resp), 0, NULL, NULL);
                if (n < 12)
                {
                    svcSleepThread(25ULL * 1000 * 1000);
                    continue;
                }
                if (resp[0] != (u8)(txid >> 8) || resp[1] != (u8)txid || (resp[3] & 0x0F) != 0)
                    break;
                int an = (resp[6] << 8) | resp[7];
                size_t pos = 12;
                for (int q = (resp[4] << 8) | resp[5]; q > 0 && pos < (size_t)n; q--)
                {
                    while (pos < (size_t)n && resp[pos] != 0)
                        pos += (resp[pos] & 0xC0) == 0xC0 ? 2 : resp[pos] + 1;
                    pos += 5;
                }
                for (; an > 0 && pos + 12 <= (size_t)n; an--)
                {
                    while (pos < (size_t)n && resp[pos] != 0 && (resp[pos] & 0xC0) != 0xC0)
                        pos += resp[pos] + 1;
                    pos += (pos < (size_t)n && (resp[pos] & 0xC0) == 0xC0) ? 2 : 1;
                    if (pos + 10 > (size_t)n)
                        break;
                    int type = (resp[pos] << 8) | resp[pos + 1];
                    int rdlen = (resp[pos + 8] << 8) | resp[pos + 9];
                    pos += 10;
                    if (type == 1 && rdlen == 4 && pos + 4 <= (size_t)n)
                    {
                        memcpy(&result, resp + pos, 4);
                        break;
                    }
                    pos += rdlen;
                }
                break;
            }
        }
        bsdClose(sock);
    }

    if (result != INADDR_NONE)
    {
        strncpy(cacheName, host, sizeof(cacheName) - 1);
        cacheName[sizeof(cacheName) - 1] = 0;
        cacheAddr = result;
        cacheTick = now;
        return result;
    }
    if (cacheAddr != INADDR_NONE && strcmp(cacheName, host) == 0)
        return cacheAddr;
    return INADDR_NONE;
}

int SocketTcpConnectFromPort(const char *ip, u16 port, u16 localPort)
{
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = SocketResolveHostV4(ip);

    if (addr.sin_addr.s_addr == INADDR_NONE)
    {
        NetworkSupportLine("server connection: hostname resolution failed");
        return SOCKET_INVALID;
    }

    int socket = bsdSocket(AF_INET, SOCK_STREAM, 0);
    if (socket < 0)
        return SOCKET_INVALID;

    // SO_REUSEADDR (same option SocketTcpListen already sets on the real listener) so this
    // socket is even allowed to bind to a local port another socket may already be using.
    const int optVal = 1;
    if (bsdSetSockOpt(socket, SOL_SOCKET, SO_REUSEADDR, (void *)&optVal, sizeof(optVal)) == -1)
    {
        NetworkSupportLine("server connection: setsockopt SO_REUSEADDR failed, errno=%d", g_bsdErrno);
        bsdClose(socket);
        return SOCKET_INVALID;
    }

    struct sockaddr_in localAddr;
    localAddr.sin_family = AF_INET;
    localAddr.sin_addr.s_addr = INADDR_ANY;
    localAddr.sin_port = htons(localPort);

    if (bsdBind(socket, (struct sockaddr *)&localAddr, sizeof(localAddr)) == -1)
    {
        NetworkSupportLine("server connection: bind to local port %d failed, errno=%d", (int)localPort, g_bsdErrno);
        bsdClose(socket);
        return SOCKET_INVALID;
    }

    // was a plain blocking connect with no timeout. Real bug found via
    // hardware test - a punch attempt toward a peer that's already given up (e.g. the
    // connector's own side already failed with EHOSTUNREACH) can sit in the OS's own TCP
    // connect timeout for 60+ seconds before this call ever returns, during which
    // PunchProbe.c's g_probeInProgress stays true and blocks every subsequent Y-hold/wake
    // trigger - observed on real hardware as ~75s of "wake request ignored - a probe is
    // already in progress" after a single failed punch. Bounded here the same way the
    // original relay design's SocketTcpConnect was once fixed for the same class of bug:
    // non-blocking connect, bounded poll for writability, then verify with SO_ERROR
    // (POLLOUT firing alone isn't proof the connect actually succeeded).
    if (!SocketMakeNonBlocking(socket))
    {
        NetworkSupportLine("server connection: SocketMakeNonBlocking failed, errno=%d", g_bsdErrno);
        bsdClose(socket);
        return SOCKET_INVALID;
    }

    if (bsdConnect(socket, (struct sockaddr *)&addr, sizeof(addr)) == -1 && g_bsdErrno != NX_EINPROGRESS)
    {
        NetworkSupportLine("server connection: connect failed, errno=%d", g_bsdErrno);
        bsdClose(socket);
        return SOCKET_INVALID;
    }

    PollResult pollRes = PollSocket(socket, 5000);
    if (pollRes != PollResult_CanWrite)
    {
        NetworkSupportLine("server connection: connect timed out/failed waiting for writability (pollRes=%d)", (int)pollRes);
        bsdClose(socket);
        return SOCKET_INVALID;
    }

    int sockErr = 0;
    socklen_t sockErrLen = sizeof(sockErr);
    if (bsdGetSockOpt(socket, SOL_SOCKET, SO_ERROR, &sockErr, &sockErrLen) == -1 || sockErr != 0)
    {
        NetworkSupportLine("server connection: connect completed with error, SO_ERROR=%d", sockErr);
        bsdClose(socket);
        return SOCKET_INVALID;
    }

    int optVal2 = 1;
    bsdSetSockOpt(socket, IPPROTO_TCP, TCP_NODELAY, &optVal2, sizeof(optVal2));

    if (!SocketMakeNonBlocking(socket))
    {
        bsdClose(socket);
        return SOCKET_INVALID;
    }

    return socket;
}

// This function must be called after SocketTcpAccept returns SOCKET_INVALID
// We use a weird hack, we need to figure out when the console is in sleep mode
// and reset the listening socket when it wakes up, the only way i found to get
// a meaningful error code is from poll, which will return 0 when no connection is pending
// and 1 otherwise but if Accept fails with EAGAIN, we know the console was in sleep mode.
// We should use nifm but that comes with its share of weirdness.....
static bool SocketIsListenNetDown()
{
    if (g_bsdErrno == NX_EAGAIN)
        return false;
    NifmInternetConnectionType type;
    u32 strength;
    NifmInternetConnectionStatus status;
    return HasNifm && R_SUCCEEDED(nifmGetInternetConnectionStatus(&type, &strength, &status)) &&
           status != NifmInternetConnectionStatus_Connected;
}

SocketAcceptResult SocketTcpAccept(int listenerHandle, int *outAccepted, struct sockaddr *out_addr,
                                   socklen_t *out_addrlen)
{
    struct pollfd pollinfo;
    pollinfo.fd = listenerHandle;
    pollinfo.events = POLLIN;
    pollinfo.revents = 0;

    int rc = bsdPoll(&pollinfo, 1, 0);
    if (rc > 0)
    {
        if (pollinfo.revents & POLLIN)
        {
            int accepted = bsdAccept(listenerHandle, out_addr, out_addrlen);

            if (accepted != SOCKET_INVALID)
            {
                // Set TCP_NODELAY
                int optVal = 1;
                bsdSetSockOpt(accepted, IPPROTO_TCP, TCP_NODELAY, &optVal, sizeof(optVal));
                *outAccepted = accepted;
                return SocketAcceptError_OK;
            }
            else
            {
                *outAccepted = SOCKET_INVALID;
                if (SocketIsListenNetDown())
                    return SocketAcceptError_NetDown;
                else
                    return SocketAcceptError_Fail;
            }
        }
    }

    *outAccepted = SOCKET_INVALID;
    return SocketAcceptError_Fail;
}

bool SocketUDPSendTo(int socket, const void *data, u32 size, struct sockaddr *addr, socklen_t addrlen)
{
    return bsdSendTo(socket, data, size, 0, addr, addrlen) == size;
}

static PollResult PollSocket(int socket, int timeoutMs)
{
    struct pollfd pollinfo;
    pollinfo.fd = socket;
    pollinfo.events = POLLOUT | POLLIN;
    pollinfo.revents = 0;

    int rc = bsdPoll(&pollinfo, 1, timeoutMs);

    if (rc > 0)
    {
        // This is not exactly correct, but we only care about the result in the context of the socket send helper
        if (pollinfo.revents & POLLERR)
            return PollResult_Disconnected;
        else if (pollinfo.revents & POLLHUP)
            return PollResult_Disconnected;
        // This comes first to detect disconnection before we can write
        else if (pollinfo.revents & POLLIN)
            return PollResult_CanRead;
        else if (pollinfo.revents & POLLOUT)
            return PollResult_CanWrite;

        return PollResult_Other;
    }

    return rc < 0 ? PollResult_Disconnected : PollResult_Timeout;
}

bool (*SocketSendReadableHook)(int socket) = NULL;

static bool SocketSendAllImpl(int sock, const void *buffer, u32 size)
{
    if (sock == SOCKET_INVALID)
        return false;
    u32 sent = 0, inputProgress = 0;
    const u64 begin = svcGetSystemTick();
    u64 progress = begin;
    while (sent < size)
    {
        if (armTicksToNs(svcGetSystemTick() - begin) >= 5000000000ULL)
            return false;
        const u32 remaining = size - sent;
        const u32 chunk = remaining > 32768 ? 32768 : remaining;
        const int res = bsdSend(sock, (const char *)buffer + sent, chunk, MSG_DONTWAIT);
        const int sendErrno = g_bsdErrno; // capture before a hook/log can issue BSD IPC
        if (res > 0)
        {
            if ((u32)res > chunk)
                return false;
            sent += (u32)res;
            progress = svcGetSystemTick();
            if (sent - inputProgress >= 16384 || sent == size)
            {
                inputProgress = sent;
                const bool alive = !SocketSendReadableHook || SocketSendReadableHook(sock);
                if (!alive)
                    return false;
            }
            continue;
        }
        if (res == 0 || sendErrno != NX_EAGAIN)
        {
            return false;
        }
        const bool alive = !SocketSendReadableHook || SocketSendReadableHook(sock);
        if (!alive)
            return false;
        const PollResult poll = PollSocket(sock, 10);
        const u64 now = svcGetSystemTick();
        if (poll == PollResult_Disconnected || poll == PollResult_Other ||
            armTicksToNs(now - progress) >= 10000000000ULL)
        {
            return false;
        }
        // Readable/writable can be spurious. Yield instead of spinning, then
        // attempt the actual send again even when poll saw no writable event.
        svcSleepThread(1000000ULL);
    }
    return sent == size;
}

bool SocketSendControlAll(int sock, const void *buffer, u32 size)
{
    return SocketSendAllImpl(sock, buffer, size);
}

// One writer per socket; reservations are bounded to4KiB and refunded on partial
// writes/EAGAIN. No shared budget lock is held across a firmware BSD call.
bool SocketSendMediaPaced(int socket, const void *buffer, u32 size, bool (*keepSending)(void *), void *context,
                          const SocketMediaBudget *budget, u64 *pacingNs)
{
    if (pacingNs)
        *pacingNs = 0;
    if (socket == SOCKET_INVALID || !budget || !budget->reserve || !budget->refund)
        return false;
    u32 sent = 0, retries = 0;
    const u64 begin = svcGetSystemTick();
    u64 progress = begin, paced = 0;
    u64 sendNs = 0, pollNs = 0, maxSendNs = 0, maxPollNs = 0;
    bool ok = false;
    while (sent < size && (keepSending != NULL || IsThreadRunning))
    {
        if (keepSending && !keepSending(context))
            break;
        if (armTicksToNs(svcGetSystemTick() - progress) >= 10000000000ULL)
            break;
        const u32 chunk = budget->reserve(context, size - sent);
        if (!chunk)
        {
            const u64 before = svcGetSystemTick();
            svcSleepThread(1000000ULL);
            paced += armTicksToNs(svcGetSystemTick() - before);
            continue;
        }
        if (chunk > size - sent || chunk > 4096u)
            break;
        const u64 sendBegin = svcGetSystemTick();
        const int n = bsdSend(socket, (const u8 *)buffer + sent, chunk, MSG_DONTWAIT);
        const int error = g_bsdErrno;
        const u64 sendTime = armTicksToNs(svcGetSystemTick() - sendBegin);
        sendNs += sendTime;
        if (sendTime > maxSendNs)
            maxSendNs = sendTime;
        if (n > 0 && (u32)n <= chunk)
        {
            budget->refund(context, chunk - (u32)n, (u32)n);
            sent += (u32)n;
            progress = svcGetSystemTick();
            continue;
        }
        budget->refund(context, chunk, 0);
        if (n == 0 || n > 0 || error != NX_EAGAIN)
            break;
        ++retries;
        struct pollfd pfd = {.fd = socket, .events = POLLOUT, .revents = 0};
        const u64 pollBegin = svcGetSystemTick();
        const int ready = bsdPoll(&pfd, 1, 10);
        const u64 pollTime = armTicksToNs(svcGetSystemTick() - pollBegin);
        pollNs += pollTime;
        if (pollTime > maxPollNs)
            maxPollNs = pollTime;
        if (ready < 0 || (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)))
            break;
        if (ready > 0)
            svcSleepThread(1000000ULL);
    }
    ok = sent == size;
    if (pacingNs)
        *pacingNs = paced;
    const u64 elapsed = armTicksToNs(svcGetSystemTick() - begin);
    if (elapsed > 50000000ULL)
    {
        NetworkSupportLine("TCP write bytes=%u wall_us=%u pace_us=%u send_us=%u poll_us=%u max_send_us=%u max_poll_us=%u retries=%u",
                           sent, (u32)(elapsed / 1000), (u32)(paced / 1000), (u32)(sendNs / 1000),
                           (u32)(pollNs / 1000), (u32)(maxSendNs / 1000), (u32)(maxPollNs / 1000), retries);
    }
    return ok;
}

s32 SocketTryRecv(int socket, void *buffer, u32 size)
{
    ssize_t r = bsdRecv(socket, buffer, size, MSG_DONTWAIT);
    // A zero-length recv on a nonzero request means the peer closed its send side.
    // Keep 0 reserved for EAGAIN so stream loops can distinguish an idle socket
    // from a disconnected one even when no video frames are being sent.
    if (r == 0 && size != 0)
        return -1;
    if (r == -1)
    {
        if (g_bsdErrno == NX_EAGAIN)
        {
            // svcSleepThread() takes nanoseconds - this was "2" (2ns, effectively no sleep
            // at all) instead of the presumably-intended 1-2ms, turning every EAGAIN into
            // an immediate retry. SocketRecevExact (the only caller) is only ever used to
            // wait for the one-time handshake exchange, never in the per-frame streaming
            // loop, so this cost
            // nothing in bandwidth but meant any socket sitting in a real "waiting for the
            // other side to send something" state - e.g. RelayMode.c's Relay_DoHandshake
            // blocking indefinitely until a relay connector shows up - busy-looped calling
            // bsdRecv as fast as IPC allowed, for the entire wait. With two such threads
            // (video+audio) and a relay connector that might not show up for a long time
            // (unlike LAN mode's near-instant local connect), this was a sustained IPC/CPU
            // hog - the likely real cause of the hbmenu-adjacent hang/lockup this session,
            // not capture.c or the hidplus timing fix (both reasonable cleanups, not this).
            return 0;
        }
        else
            return -1;
    }
    return (s32)r;
}

s32 SocketRecv(int socket, void *buffer, u32 size)
{
    const s32 result = SocketTryRecv(socket, buffer, size);
    if (result == 0)
        svcSleepThread(1000000ULL); // handshake retries retain yielding
    return result;
}

bool SocketMakeNonBlocking(int socket)
{
    return bsdFcntl(socket, F_SETFL, NX_O_NONBLOCK) != -1;
}

int SocketNativeErrno()
{
    return g_bsdErrno;
}

