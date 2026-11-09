#include "PunchConnector.hpp"
#include "ConnectorMedia.hpp"
#include "AudioPlayer.hpp"
#include "UdpVideoSession.hpp"
#include "DnsClient.hpp"
#include "TicketLogRedaction.hpp"

#include <switch.h>
#include <switch/crypto/sha256.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace Network::Punch
{

namespace
{

// ---- shared state -----------------------------------------------------------------------
std::atomic<int> g_state{(int)State::Idle};
std::mutex g_mutex; // guards the strings, the queue and the callbacks
std::string g_peerDescription, g_errorMessage;
void (*g_videoCb)(uint8_t *, size_t, void *) = nullptr;
void *g_videoUser = nullptr;
void (*g_stillCb)(uint8_t *, size_t, void *) = nullptr;
void *g_stillUser = nullptr;

struct Frame
{
    std::vector<uint8_t> data;
    bool still;
};
std::deque<Frame> g_queue;
std::atomic<bool> g_needResync{false};
std::atomic<bool> g_requestKeyframe{false};
std::atomic<int> g_backlogDrops{
    0}; //  telemetry: times the receive backlog hit MaxQueuedFrames and was dropped
constexpr size_t MaxQueuedFrames = 90;

std::thread g_worker;
std::atomic<bool> g_cancel{false};
std::atomic<int> g_sock{-1}; // the punched socket while streaming
std::mutex g_sendMutex;

// connection ticket from "PUNCHJOIN2" ("OK <host> <ticket>"). Sent to the console in the Reserved
// bytes (offsets 10..15) of the video handshake request so it accepts this connection even when NAT changed
// its source address (a join from inside the console's own house). Only used on the internet path.
uint8_t g_connTicket[6] = {};
bool g_haveConnTicket = false;

// input edge-tracking (UI thread only)
bool g_prevTouch = false;
float g_lastTx = 0, g_lastTy = 0;
double g_lastTouchSend = 0;
uint64_t g_lastButtons = ~0ull;
int32_t g_lastSticks[4] = {1 << 30, 0, 0, 0};

// Same logging style as the Android app's PunchSession.log(): every step is written, with a
// timestamp, to a crash-durable file on the SD card (open/append/close per line) and to the
// UDP debug collector; "Say" lines are also shown on the Connect screen as the current status.
// File: /config/nxremoteplay/nro_log.txt - read it over FTP after a test.
void Log(const std::string &line)
{
    if (FILE *f = fopen("/config/nxremoteplay/nro_log.txt", "a"))
    {
        time_t t = time(nullptr);
        tm lt{};
        localtime_r(&t, &lt);
        fprintf(f, "%02d:%02d:%02d %s\n", lt.tm_hour, lt.tm_min, lt.tm_sec, line.c_str());
        fclose(f);
    }
}

std::string g_status; // current step text for the Connect screen, guarded by g_mutex
void Say(const std::string &line)
{
    Log(line);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_status = line;
}

void SetState(State s)
{
    g_state.store((int)s);
}

void Fail(const std::string &message)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_errorMessage = message;
    }
    Log(message);
    SetState(State::Error);
}

bool Cancelled()
{
    return g_cancel.load();
}

// ---- socket helpers ---------------------------------------------------------------------
void SetTimeouts(int s, int ms)
{
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

// TCP socket bound to localPort (SO_REUSEADDR) so a later connect() can reuse the same port
// the Rendezvous server observed - required for simultaneous open.
int MakeBoundTcp(uint16_t localPort)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return -1;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
    setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(localPort);
    local.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, (sockaddr *)&local, sizeof(local)) != 0)
    {
        close(s);
        return -1;
    }
    return s;
}

// connect() with a timeout (non-blocking + select), then back to blocking mode.
int g_lastConnectErrno = 0; // why the last ConnectTimeout() failed (0 = timed out / cancelled); worker thread only

bool ConnectTimeout(int s, const sockaddr_in &addr, int timeoutMs)
{
    g_lastConnectErrno = 0;
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
    int rc = connect(s, (const sockaddr *)&addr, sizeof(addr));
    if (rc != 0 && errno != EINPROGRESS && errno != EWOULDBLOCK && errno != EAGAIN)
    {
        g_lastConnectErrno = errno;
        return false;
    }
    if (rc != 0)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (true)
        {
            if (Cancelled())
                return false;
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(s, &wfds);
            timeval tv{0, 200 * 1000};
            int sel = select(s + 1, nullptr, &wfds, nullptr, &tv);
            if (sel > 0)
            {
                int err = 0;
                socklen_t len = sizeof(err);
                getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len);
                if (err != 0)
                {
                    g_lastConnectErrno = err;
                    return false;
                }
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
        }
    }
    fcntl(s, F_SETFL, flags & ~O_NONBLOCK);
    return true;
}

bool ResolveIPv4(const std::string &host, uint16_t port, sockaddr_in &out)
{
    out = {};
    out.sin_family = AF_INET;
    out.sin_port = htons(port);
    in_addr a{};
    if (inet_aton(host.c_str(), &a) == 0)
        return false; // IPv4 literal only (no DNS on this path)
    out.sin_addr = a;
    return true;
}

// A server given by NAME is resolved on the worker thread (Dns::ResolveBlocking); empty when it was an IPv4 literal.
static std::string g_serverName;

bool RecvExact(int s, uint8_t *buf, size_t n)
{
    size_t got = 0;
    while (got < n)
    {
        if (Cancelled())
            return false;
        ssize_t r = recv(s, buf + got, n - got, 0);
        if (r > 0)
        {
            got += (size_t)r;
            continue;
        }
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        {
            // SO_RCVTIMEO expired with nothing: treat a whole timeout of silence as dead.
            return false;
        }
        return false; // 0 = peer closed, <0 = error
    }
    return true;
}

bool SendAll(int s, const uint8_t *buf, size_t n)
{
    size_t sent = 0;
    while (sent < n)
    {
        ssize_t r = send(s, buf + sent, n - sent, 0);
        if (r <= 0)
            return false;
        sent += (size_t)r;
    }
    return true;
}

// One "\n"-terminated line (TCP), with the socket's receive timeout as the deadline.
bool RecvLine(int s, std::string &out)
{
    out.clear();
    char c;
    while (out.size() < 200)
    {
        ssize_t r = recv(s, &c, 1, 0);
        if (r <= 0)
            return !out.empty();
        if (c == '\n')
            return true;
        if (c != '\r')
            out.push_back(c);
    }
    return false;
}

// ---- the worker -------------------------------------------------------------------------
std::string SendWake(const sockaddr_in &server, const std::string &id, const std::string &pw)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return "no socket";
    SetTimeouts(s, 3000);
    std::string msg = "PUNCHWAKE " + id + " " + pw + "\n";
    sendto(s, msg.data(), msg.size(), 0, (const sockaddr *)&server, sizeof(server));
    char buf[64] = {};
    ssize_t r = recv(s, buf, sizeof(buf) - 1, 0);
    close(s);
    if (r <= 0)
        return "no reply";
    std::string reply(buf, (size_t)r);
    while (!reply.empty() && (reply.back() == '\n' || reply.back() == '\r'))
        reply.pop_back();
    return reply;
}

void PushFrame(std::vector<uint8_t> &&data, bool still)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_queue.size() >= MaxQueuedFrames)
    {
        // Decoder can't keep up: drop the backlog and re-sync on the next keyframe rather than
        // growing latency (a P-frame gap would corrupt the picture until then).
        g_queue.clear();
        g_needResync.store(true);
        g_backlogDrops.fetch_add(1);
        g_requestKeyframe.store(true);
    }
    g_queue.push_back({std::move(data), still});
}

#include "StreamLoop.inc"

// ---- LAN discovery (no server needed) ---------------------------------------------------
// client -> UDP broadcast :9913 "NXLAN1 DISCOVER <id>"; the host answers "NXLAN1 HERE <id> <port>"
// and listens on that TCP port for ~8 s; then "NXLAN1 CHALLENGE <nonce>" / "NXLAN1 AUTH <cn> <sha256>"
// proves the password without sending it (see PunchProbe.c's LanHost for the host side).
constexpr uint16_t LanUdpPort = 9913;

void LanToHex(const uint8_t *in, size_t n, char *out)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++)
    {
        out[i * 2] = hex[in[i] >> 4];
        out[i * 2 + 1] = hex[in[i] & 0xF];
    }
    out[n * 2] = 0;
}

std::string LanNonce()
{
    static uint64_t counter = 0x9E3779B97F4A7C15ull;
    counter = counter * 6364136223846793005ull + armGetSystemTick() + 1442695040888963407ull;
    uint64_t v = counter ^ (counter >> 29);
    uint8_t bytes[8];
    for (int i = 0; i < 8; i++)
    {
        bytes[i] = (uint8_t)(v & 0xFF);
        v >>= 8;
    }
    char out[17];
    LanToHex(bytes, 8, out);
    return out;
}

// Result of a LAN attempt: >= 0 = an authenticated, connected socket ready for the NxRemotePlay
// handshake; -1 = nobody answered (fall back to the server); -2 = a console answered but the
// password was wrong (error already reported).
int TryLan(const std::string &id, const std::string &pw)
{
    int udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp < 0)
        return -1;
    int one = 1;
    setsockopt(udp, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    SetTimeouts(udp, 250);

    // Broadcast targets: the limited broadcast (doesn't always work on Switch) and the directed
    // broadcast of our own subnet from nifm (addr | ~mask), which does.
    std::vector<uint32_t> targets = {htonl(INADDR_BROADCAST)};
    u32 ownAddr = 0; // network byte order as nifm reports it; used for the unicast sweep below
    if (R_SUCCEEDED(nifmInitialize(NifmServiceType_User)))
    {
        u32 addr = 0, mask = 0, gw = 0, d1 = 0, d2 = 0;
        if (R_SUCCEEDED(nifmGetCurrentIpConfigInfo(&addr, &mask, &gw, &d1, &d2)) && mask != 0)
        {
            targets.push_back((uint32_t)(addr | ~mask));
            ownAddr = addr;
        }
        nifmExit();
    }

    const std::string discover = "NXLAN1 DISCOVER " + id + "\n";
    sockaddr_in host{};
    uint16_t hostPort = 0;
    bool found = false, busy = false;
    for (int round = 0; round < 6 && !found && !busy && !Cancelled(); round++) // ~1.5 s
    {
        for (uint32_t t : targets)
        {
            sockaddr_in to{};
            to.sin_family = AF_INET;
            to.sin_port = htons(LanUdpPort);
            to.sin_addr.s_addr = t;
            sendto(udp, discover.data(), discover.size(), 0, (const sockaddr *)&to, sizeof(to));
        }
        char buf[96] = {};
        sockaddr_in from{};
        socklen_t fl = sizeof(from);
        ssize_t r = recvfrom(udp, buf, sizeof(buf) - 1, 0, (sockaddr *)&from, &fl);
        if (r > 0)
        {
            std::string reply(buf, (size_t)r);
            const std::string prefix = "NXLAN1 HERE " + id + " ";
            if (reply.rfind(prefix, 0) == 0)
            {
                hostPort = (uint16_t)atoi(reply.c_str() + prefix.size());
                host = from;
                host.sin_port = htons(hostPort);
                found = hostPort != 0;
            }
            else if (reply == "NXLAN1 BUSY " + id)
                busy = true;
        }
    }
    // nobody answered the broadcasts - ask every address of our /24 directly (broadcast
    // frames are filtered/not forwarded on some networks; unicast still works).
    if (!found && !busy && ownAddr != 0 && !Cancelled())
    {
        uint8_t o[4];
        memcpy(o, &ownAddr, 4); // bytes in network order: o[0].o[1].o[2].o[3]
        Log("LAN: no broadcast answer - sweeping the local /24");
        for (int h = 1; h <= 254; h++)
        {
            if (h == o[3])
                continue;
            sockaddr_in to{};
            to.sin_family = AF_INET;
            to.sin_port = htons(LanUdpPort);
            uint8_t b[4] = {o[0], o[1], o[2], (uint8_t)h};
            memcpy(&to.sin_addr.s_addr, b, 4);
            sendto(udp, discover.data(), discover.size(), 0, (const sockaddr *)&to, sizeof(to));
        }
        for (int i = 0; i < 6 && !found && !busy && !Cancelled(); i++) // ~1.5 s (250 ms timeouts)
        {
            char buf[96] = {};
            sockaddr_in from{};
            socklen_t fl = sizeof(from);
            ssize_t r = recvfrom(udp, buf, sizeof(buf) - 1, 0, (sockaddr *)&from, &fl);
            if (r <= 0)
                continue;
            std::string reply(buf, (size_t)r);
            const std::string prefix = "NXLAN1 HERE " + id + " ";
            if (reply.rfind(prefix, 0) == 0)
            {
                hostPort = (uint16_t)atoi(reply.c_str() + prefix.size());
                host = from;
                host.sin_port = htons(hostPort);
                found = hostPort != 0;
            }
            else if (reply == "NXLAN1 BUSY " + id)
                busy = true;
        }
    }
    close(udp);
    if (busy)
    {
        Fail("The console is already connected to another device.");
        return -2;
    }
    if (!found)
        return -1;

    char ip[32];
    inet_ntop(AF_INET, &host.sin_addr, ip, sizeof(ip));
    Log(std::string("LAN: console answered at ") + ip + ":" + std::to_string(hostPort));
    Say("Found the Switch on this network. Connecting...");

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return -1;
    if (!ConnectTimeout(s, host, 5000))
    {
        close(s);
        Log("LAN: couldn't connect to the console's LAN port");
        return -1;
    }
    SetTimeouts(s, 5000);

    std::string line;
    RecvLine(s, line);
    if (line.rfind("NXLAN1 CHALLENGE ", 0) != 0 || line.size() != 17 + 16)
    {
        close(s);
        Log("LAN: unexpected challenge '" + line + "'");
        return -1;
    }
    const std::string hostNonce = line.substr(17);
    const std::string clientNonce = LanNonce();
    const std::string material = pw + ":" + hostNonce + ":" + clientNonce;
    uint8_t digest[32];
    sha256CalculateHash(digest, material.data(), material.size());
    char hashHex[65];
    LanToHex(digest, 32, hashHex);
    const std::string auth = "NXLAN1 AUTH " + clientNonce + " " + hashHex + "\n";
    SendAll(s, (const uint8_t *)auth.data(), auth.size());

    RecvLine(s, line);
    if (line != "NXLAN1 OK")
    {
        close(s);
        Fail("A Switch with that Session ID is on this network, but the password was wrong.");
        return -2;
    }
    Log("LAN: authenticated");
    return s;
}

// ---- NxRemotePlay handshake + stream over an established, authenticated socket -----------------
void RunSession(int s, const std::string &via, const sockaddr_in &server)
{
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    SetTimeouts(s, 15000);
    {
        int rcvBuf = 0;
        socklen_t rcvLen = sizeof(rcvBuf);
        if (getsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcvBuf, &rcvLen) == 0)
            Log("Video: TCP receive buffer (SO_RCVBUF) = " + std::to_string(rcvBuf) + " bytes");
        else
            Log("Video: TCP receive buffer (SO_RCVBUF) query failed");
    }

    // NxRemotePlay video handshake (hello 10 B, request 16 B, response 72 B; code 6 = OK)
    Log("NXRP receiver for ");
    Say("Starting the video stream...");
    uint8_t hello[16] = {};
    // MetaFlags 0x03 = video + audio, AudioBatching 1 (8 KiB PCM packets, ~43 ms) - the same request the Android app
    // makes (console and desktop bridge both accept it; a host that cannot send audio just never sends any).
    uint8_t request[16];
    BuildStreamRequest(request, server.sin_family == AF_INET,
                       via == "internet" && g_haveConnTicket ? g_connTicket : nullptr);
    uint8_t response[72];
    if (via == "internet" && g_haveConnTicket)
    {
        memcpy(request + 10, g_connTicket, sizeof(g_connTicket));
        Log("Video: connection ticket included in the handshake request");
    }
    if (!RecvExact(s, hello, sizeof(hello)))
    {
        close(s);
        if (Cancelled())
            return;
        return Fail("Video: never received hello from the console.");
    }
    Log(std::string("Video: hello = ") + std::string((const char *)hello, strnlen((const char *)hello, sizeof(hello))));
    if (!SendAll(s, request, sizeof(request)) || !RecvExact(s, response, sizeof(response)))
    {
        close(s);
        if (Cancelled())
            return;
        return Fail("Video: never received the handshake response from the console.");
    }
    uint32_t code = response[0] | (response[1] << 8) | (response[2] << 16) | ((uint32_t)response[3] << 24);
    if (code != 6)
    {
        close(s);
        return Fail("Video: the console rejected the handshake (code " + std::to_string(code) + ").");
    }

    g_sock.store(s);
    SetState(State::Connected);
    Log("Video: handshake OK - streaming (" + via + ")");
    NroAudio::Start(&Log); // non-fatal: without audout the stream just stays silent
    StreamLoop(s, response, server, via == "LAN");

    // stream over
    NroAudio::Stop();
    g_sock.store(-1);
    {
        std::lock_guard<std::mutex> lock(g_sendMutex);
        close(s);
    }
    if (Cancelled())
        return;
    Fail("The connection to the console ended.");
}

void Worker(sockaddr_in server, std::string id, std::string pw, bool tryLan)
{
    const uint16_t localPort = (uint16_t)(20000 + (int)(armGetSystemTick() % 40000));

    // A server given by NAME is resolved first, here on the worker thread (never on the UI thread); if the lookup
    // fails the same-network search below still runs and the server step reports the lookup failure.
    if (!g_serverName.empty())
    {
        uint32_t ip = 0;
        if (Dns::ResolveBlocking(g_serverName, ip))
        {
            server.sin_family = AF_INET;
            server.sin_addr.s_addr = ip;
            Log("Server " + g_serverName + " -> " + inet_ntoa(server.sin_addr));
        }
        else
            server = sockaddr_in{};
    }

    // 0. same network? (no server needed, and the only thing that works when both Switches sit
    //    behind one router) - a couple of seconds, then on to the server path.
    if (tryLan)
    {
        Say("Looking for the Switch on this network...");
        int lan = TryLan(id, pw);
        if (lan >= 0)
        {
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_peerDescription = "local network";
            }
            SetState(State::Handshaking);
            return RunSession(lan, "LAN", server);
        }
        if (lan == -2 || Cancelled())
            return;
        Log("LAN: no console answered - trying the server");
        Say("Contacting the server...");
    }
    if (server.sin_family == 0 && !g_serverName.empty())
        return Fail("Couldn't look up the server name '" + g_serverName +
                    "'. Check the connection and the server address in Settings.");
    if (server.sin_family == 0)
        return Fail(
            "No Switch answered on this network, and the server address isn't valid. Use an IP address or a name, like play.nxremoteplay.com or 203.0.113.5:7921.");

    // 1. wake the console
    Say("Waking the Switch...");
    std::string wake = SendWake(server, id, pw);
    Log("PUNCHWAKE: server replied '" + wake + "'");
    if (wake == "ERR NOTFOUND")
        return Fail(
            "No console is registered with that Session ID and Password. Check them on the Switch's Host screen.");
    if (wake == "no reply" || wake == "no socket")
        return Fail("Couldn't reach the server. Check the server address in Settings and that the server is running.");

    // 2. PUNCHJOIN until the console has registered for the punch (it picks the wake up on its
    //    next REFRESH, <= 30 s, so keep trying comfortably longer than that).
    std::string reply;
    const auto joinDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
    int attempt = 0;
    bool useTicketJoin = true; // PUNCHJOIN2 first; an older server answers ERR BADCMD and we retry with PUNCHJOIN
    int connectFailures = 0;
    int lastFailErrno = 0;
    uint16_t joinPort =
        localPort; // the port the server saw for the PUNCHJOIN that got an OK - the punch must reuse exactly this one
    while (std::chrono::steady_clock::now() < joinDeadline)
    {
        if (Cancelled())
            return;
        ++attempt;
        // Waiting retries (host not registered yet) use a fresh local port: re-binding the port the
        // previous attempt just closed can fail instantly while that connection is still winding down.
        if (attempt > 1)
            joinPort = (uint16_t)(20000 + (int)((armGetSystemTick() + attempt * 7919) % 40000));
        int r = MakeBoundTcp(joinPort);
        if (r < 0)
        {
            Log("Attempt " + std::to_string(attempt) + ": couldn't bind local port " + std::to_string(joinPort) +
                " (errno " + std::to_string(errno) + ")");
            if (attempt == 1)
                return Fail("Couldn't open a local socket.");
            ++connectFailures;
            lastFailErrno = errno;
            for (int i = 0; i < 15 && !Cancelled(); i++)
                svcSleepThread(100ULL * 1000 * 1000);
            continue;
        }
        const auto connectStart = std::chrono::steady_clock::now();
        if (!ConnectTimeout(r, server, 10000))
        {
            close(r);
            if (Cancelled())
                return;
            const int err = g_lastConnectErrno;
            const long long ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - connectStart)
                    .count();
            Log("Attempt " + std::to_string(attempt) + ": connect to the server from local port " +
                std::to_string(joinPort) + " failed after " + std::to_string(ms) + " ms, errno " + std::to_string(err) +
                (err ? std::string(" (") + strerror(err) + ")" : std::string(" (timed out)")));
            // the very first connect failing means the server really can't be reached; once it has
            // answered, a failed retry is a local hiccup - keep waiting for the host instead.
            if (attempt == 1)
                return Fail(
                    "Couldn't reach the server (TCP). Check the server address and that TCP is allowed through.");
            ++connectFailures;
            lastFailErrno = err;
            Say("Waiting for the Switch to respond...");
            for (int i = 0; i < 15 && !Cancelled(); i++)
                svcSleepThread(100ULL * 1000 * 1000);
            continue;
        }
        SetTimeouts(r, 10000);
        std::string cmd = std::string(useTicketJoin ? "PUNCHJOIN2 " : "PUNCHJOIN ") + id + " " + pw + "\n";
        SendAll(r, (const uint8_t *)cmd.data(), cmd.size());
        reply.clear();
        RecvLine(r, reply);
        close(r); // free the local port for the punch (or the next retry)
        if (useTicketJoin && reply == "ERR BADCMD")
        {
            useTicketJoin = false;
            Log("Server does not know PUNCHJOIN2 - joining the old way (no connection ticket)");
            continue; // retry at once; the next attempt uses a fresh local port
        }
        if (!reply.empty() && reply != "ERR NOTFOUND")
            break;
        Say("Waiting for the Switch to respond...");
        Log("Attempt " + std::to_string(attempt) + ": server replied '" + reply +
            "' - host not ready yet, retrying...");
        for (int i = 0; i < 15 && !Cancelled(); i++)
            svcSleepThread(100ULL * 1000 * 1000); // 1.5 s
    }
    if (Cancelled())
        return;
    Log("Server replied: " + RedactTicketReply(reply));
    if (reply.rfind("OK ", 0) != 0)
    {
        if (reply.empty() && connectFailures > 0)
            return Fail("Couldn't reach the server (TCP) after the first attempt (errno " +
                        std::to_string(lastFailErrno) + "). Check the server address and that TCP is allowed through.");
        return Fail(
            "The console didn't answer. Make sure it's on with Hosting enabled, and not already connected to another device.");
    }

    std::string hostAddr = reply.substr(3);
    g_haveConnTicket = false;
    {
        const size_t sp = hostAddr.find(' ');
        if (sp != std::string::npos)
        {
            const std::string tok = hostAddr.substr(sp + 1);
            hostAddr.resize(sp);
            if (tok.size() == 12)
            {
                bool ok = true;
                uint8_t t[6] = {};
                for (size_t i = 0; i < 12 && ok; i++)
                {
                    const char c = tok[i];
                    const int v = (c >= '0' && c <= '9')   ? c - '0'
                                  : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                                  : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                                                           : -1;
                    if (v < 0)
                    {
                        ok = false;
                        break;
                    }
                    if (i % 2 == 0)
                        t[i / 2] = (uint8_t)(v << 4);
                    else
                        t[i / 2] |= (uint8_t)v;
                }
                if (ok)
                {
                    memcpy(g_connTicket, t, sizeof(t));
                    g_haveConnTicket = true;
                }
            }
        }
        Log(g_haveConnTicket ? "Connection ticket received (the console accepts this join from any network address)"
                             : "No connection ticket (older server or console: the console checks the source address)");
    }
    size_t colon = hostAddr.rfind(':');
    sockaddr_in consoleAddr{};
    if (colon == std::string::npos ||
        !ResolveIPv4(hostAddr.substr(0, colon), (uint16_t)atoi(hostAddr.c_str() + colon + 1), consoleAddr))
        return Fail("Couldn't parse the console's address '" + hostAddr + "'.");

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_peerDescription = hostAddr;
    }
    SetState(State::Paired);

    // 3. punch: connect() to the console from the same local port
    SetState(State::Handshaking);
    Say("Connecting to the Switch...");
    Log("Punching toward " + hostAddr + " from local port " + std::to_string(joinPort) + "...");
    int s = MakeBoundTcp(joinPort);
    if (s < 0)
        return Fail("Couldn't open a local socket for the direct connection.");
    if (!ConnectTimeout(s, consoleAddr, 15000))
    {
        close(s);
        if (Cancelled())
            return;
        return Fail(
            "Couldn't connect to the Switch. The network in use may be blocking direct connections - try a different network or mobile data.");
    }
    Log("PUNCH SUCCEEDED - direct connection to " + hostAddr + " established.");
    RunSession(s, "internet", server);
}

void JoinWorker()
{
    if (g_worker.joinable())
        g_worker.join();
}

} // namespace

void Connect(const std::string &serverHost, uint16_t serverPort, const std::string &id, const std::string &password)
{
    const State st = (State)g_state.load();
    if (st == State::Requesting || st == State::Paired || st == State::Handshaking || st == State::Connected)
        return;

    JoinWorker();
    g_cancel.store(false);
    g_needResync.store(false);
    g_requestKeyframe.store(false);
    g_haveConnTicket = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_queue.clear();
        g_errorMessage.clear();
        g_peerDescription.clear();
        g_status.clear();
    }
    g_prevTouch = false;
    g_lastButtons = ~0ull;
    g_lastSticks[0] = 1 << 30;
    ConnectorMedia::NewSession(); // forget the previous session's last picture (video or still)

    // The server is optional now: the LAN search in Worker needs none. A bad/missing server
    // address only matters if the LAN search finds nobody (Worker fails then).
    sockaddr_in server{};
    g_serverName.clear();
    if (!ResolveIPv4(serverHost, serverPort, server))
    {
        server = sockaddr_in{};
        if (!serverHost.empty())
        {
            // A name: resolved later on the worker thread, never on the UI thread.
            g_serverName = serverHost;
            server.sin_family = AF_INET;
            server.sin_port = htons(serverPort);
        }
    }

    Log("=== Join " + id + " via " + serverHost + ":" + std::to_string(serverPort) + " ===");
    Say("Starting...");
    SetState(State::Requesting);
    g_worker = std::thread(Worker, server, id, password, true);
}

static void ServiceHomeRelease();

void Poll()
{
    const State st = (State)g_state.load();
    if (st == State::Connected)
        ServiceHomeRelease();
    static std::deque<Frame> pendingVideo; // UI thread only: H.264 AUs received but not yet shown
    if (st != State::Connected && st != State::Error)
    {
        pendingVideo.clear();
        return;
    }

    std::deque<Frame> frames;
    void (*videoCb)(uint8_t *, size_t, void *);
    void *videoUser;
    void (*stillCb)(uint8_t *, size_t, void *);
    void *stillUser;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        frames.swap(g_queue);
        videoCb = g_videoCb;
        videoUser = g_videoUser;
        stillCb = g_stillCb;
        stillUser = g_stillUser;
    }
    if (g_needResync.exchange(false))
    {
        ConnectorMedia::ResetVideoSync();
        pendingVideo.clear();
    }
    // JPEG stills (console on HOME): only the NEWEST matters, and decoding + uploading each one
    // costs tens of ms on the UI thread. Decoding every queued still fell further behind every
    // frame (the console sends ~24/s), which froze the whole UI - the picture, the passthrough
    // combo, everything (hardware-observed , HOME only; hbmenu/games send small H.264
    // and were fine). H.264 access units all still go through, in order.
    long lastStill = -1, lastVideo = -1;
    for (size_t i = 0; i < frames.size(); i++)
    {
        if (frames[i].still)
            lastStill = (long)i;
        else
            lastVideo = (long)i;
    }
    // Paced presentation . Every H.264 access unit is still decoded in order (P-frames
    // need their references), but now ONE access unit is shown per UI loop instead of only the newest of
    // each batch: a burst of 3 AUs that arrived together used to show 1 and silently drop 2 (judder, not
    // lag). A burst now plays out over the next 2-3 loops; with 30 fps video on a 60 Hz loop the queue
    // drains twice as fast as it fills, so steady-state latency is unchanged. Only if more than
    // kMaxShowBacklog AUs are waiting are the oldest decoded without being shown (real pile-up).
    constexpr size_t kMaxShowBacklog = 3;
    const u64 pollStart = armGetSystemTick();
    for (size_t i = 0; i < frames.size(); i++)
    {
        Frame &f = frames[i];
        if (f.still)
        {
            if ((long)i == lastStill && stillCb)
                stillCb(f.data.data(), f.data.size(), stillUser);
        }
        else if (videoCb)
            pendingVideo.push_back(std::move(f));
    }
    static int winSkipped = 0, winShown = 0, winMaxPending = 0, winGapHist[4] = {0, 0, 0, 0}, loopsSinceShown = 0;
    static double winMaxShowGapMs = 0;
    static u64 lastShownTick = 0;
    if (videoCb)
    {
        if ((int)pendingVideo.size() > winMaxPending)
            winMaxPending = (int)pendingVideo.size();
        while (pendingVideo.size() > kMaxShowBacklog)
        {
            ConnectorMedia::SetPresentFrames(false);
            videoCb(pendingVideo.front().data.data(), pendingVideo.front().data.size(), videoUser);
            pendingVideo.pop_front();
            ++winSkipped;
        }
        ++loopsSinceShown;
        if (!pendingVideo.empty())
        {
            ConnectorMedia::SetPresentFrames(true);
            videoCb(pendingVideo.front().data.data(), pendingVideo.front().data.size(), videoUser);
            pendingVideo.pop_front();
            ++winShown;
            ++winGapHist[loopsSinceShown >= 4 ? 3 : (loopsSinceShown < 1 ? 0 : loopsSinceShown - 1)];
            loopsSinceShown = 0;
            const u64 shownTick = armGetSystemTick();
            if (lastShownTick)
            {
                const double gapMs = (double)(shownTick - lastShownTick) * 1000.0 / (double)armGetSystemTickFreq();
                if (gapMs > winMaxShowGapMs)
                    winMaxShowGapMs = gapMs;
            }
            lastShownTick = shownTick;
        }
    }
    ConnectorMedia::SetPresentFrames(true);

    // Waiting for a keyframe to start (or restart) decoding: the host's own keyframe may have left before
    // the media path was open, and the next scheduled one can be seconds away, so ask for one.
    static u64 lastKeyAsk = 0;
    static unsigned winKeyAsks = 0, winDecodeErrors = 0;
    winDecodeErrors += ConnectorMedia::TakeDecoderErrors();
    static ConnectorMedia::SyncStats winSync;
    {
        const auto n = ConnectorMedia::TakeSyncStats();
        winSync.unsyncedAus += n.unsyncedAus;
        winSync.withSps += n.withSps;
        winSync.withPps += n.withPps;
        winSync.withIdr += n.withIdr;
        winSync.withSlice += n.withSlice;
        winSync.syncs += n.syncs;
        if (n.unsyncedAus)
            winSync.lastSize = n.lastSize;
    }
    if (videoCb && ConnectorMedia::WaitingForKeyframe() && (!lastKeyAsk || pollStart - lastKeyAsk > armGetSystemTickFreq() / 2))
    {
        lastKeyAsk = pollStart;
        ++winKeyAsks;
        g_requestKeyframe.store(true);
    }

    // Receive-side telemetry : one line per ~5 s in nro_log.txt so a laggy session can be
    // diagnosed from the log (UI loop rate, how many frames arrived per Poll, backlog, decode+show time).
    static u64 winStart = 0;
    static int winPolls = 0, winVideo = 0, winMaxBatch = 0;
    static double winPollMsSum = 0, winPollMsMax = 0;
    const u64 pollEnd = armGetSystemTick();
    if (!winStart)
        winStart = pollStart;
    ++winPolls;
    winVideo +=
        (int)(lastVideo >= 0 ? std::count_if(frames.begin(), frames.end(), [](const Frame &f) { return !f.still; })
                             : 0);
    if ((int)frames.size() > winMaxBatch)
        winMaxBatch = (int)frames.size();
    const double pollMs = (double)(pollEnd - pollStart) * 1000.0 / (double)armGetSystemTickFreq();
    winPollMsSum += pollMs;
    if (pollMs > winPollMsMax)
        winPollMsMax = pollMs;
    const double winSec = (double)(pollEnd - winStart) / (double)armGetSystemTickFreq();
    if (winSec >= 5.0)
    {
        char buf[220];
        snprintf(
            buf, sizeof(buf),
            "NRO perf: %.1f UI loops/s, %.1f video AU/s, max AUs per loop %d, poll avg/max %.1f/%.1f ms, backlog drops %d",
            winPolls / winSec, winVideo / winSec, winMaxBatch, winPollMsSum / (winPolls ? winPolls : 1), winPollMsMax,
            g_backlogDrops.exchange(0));
        Log(buf);
        snprintf(
            buf, sizeof(buf),
            "NRO shown: %.1f frames/s shown, %d decoded-not-shown, loops between shown frames 1:%d 2:%d 3:%d 4+:%d, max gap between shown %.0f ms, max pending %d",
            winShown / winSec, winSkipped, winGapHist[0], winGapHist[1], winGapHist[2], winGapHist[3], winMaxShowGapMs,
            winMaxPending);
        Log(buf);
        if (winKeyAsks || winDecodeErrors)
            Log("NRO video recovery: " + std::to_string(winDecodeErrors) + " decoder error(s), " +
                std::to_string(winKeyAsks) + " keyframe request(s) while waiting for a keyframe");
        if (winSync.unsyncedAus || winSync.syncs)
            Log("NRO video sync: " + std::to_string(winSync.unsyncedAus) + " access unit(s) discarded unsynced (SPS " +
                std::to_string(winSync.withSps) + ", PPS " + std::to_string(winSync.withPps) + ", IDR " +
                std::to_string(winSync.withIdr) + ", other slice " + std::to_string(winSync.withSlice) +
                ", last " + std::to_string(winSync.lastSize) + " B), synced " + std::to_string(winSync.syncs) +
                " time(s)");
        winSync = ConnectorMedia::SyncStats{};
        winKeyAsks = winDecodeErrors = 0;
        winShown = winSkipped = winMaxPending = 0;
        winGapHist[0] = winGapHist[1] = winGapHist[2] = winGapHist[3] = 0;
        winMaxShowGapMs = 0;
        winStart = pollEnd;
        winPolls = winVideo = winMaxBatch = 0;
        winPollMsSum = winPollMsMax = 0;
    }
}

void Disconnect()
{
    g_cancel.store(true);
    int s = g_sock.load();
    if (s >= 0)
        shutdown(s, SHUT_RDWR); // unblocks the worker's recv
    JoinWorker();
    g_sock.store(-1);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_queue.clear();
    }
    SetState(State::Idle);
}

State GetState()
{
    return (State)g_state.load();
}
const std::string &GetErrorMessage()
{
    return g_errorMessage;
}
std::string GetStatusText()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_status;
}
void Note(const std::string &line)
{
    Log(line);
}

void SetOnVideoFrame(void (*callback)(uint8_t *, size_t, void *), void *userdata)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_videoCb = callback;
    g_videoUser = userdata;
}

void SetOnStillFrame(void (*callback)(uint8_t *, size_t, void *), void *userdata)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_stillCb = callback;
    g_stillUser = userdata;
}

bool SendInputState(bool touchActive, float touchX, float touchY, uint64_t buttons, int32_t lx, int32_t ly, int32_t rx,
                    int32_t ry)
{
    if ((State)g_state.load() != State::Connected)
        return false;
    // Real buttons only (bits 0-15). HidNpadButton bits 18/19 are the left stick pushed right/down on
    // this side but mean HOME/CAPTURE in the host's virtual-pad state - see PunchProbe.c.
    buttons &= 0xFFFFull;
    auto clampS16 = [](int32_t v) { return (int16_t)(v > 32767 ? 32767 : (v < -32767 ? -32767 : v)); };
    std::lock_guard<std::mutex> lock(g_sendMutex);
    const int s = g_sock.load(); // load inside the same lock used for close/feedback/input
    if (s < 0)
        return false;

    // Controller: full state, only when it changes (PunchProbe.c ApplyControllerPacket).
    const int32_t sticks[4] = {lx, ly, rx, ry};
    if (buttons != g_lastButtons || memcmp(sticks, g_lastSticks, sizeof(sticks)) != 0)
    {
        g_lastButtons = buttons;
        memcpy(g_lastSticks, sticks, sizeof(sticks));
        uint8_t pkt[17];
        pkt[0] = 0x02;
        memcpy(pkt + 1, &buttons, 8);
        int16_t v[4] = {clampS16(lx), clampS16(ly), clampS16(rx), clampS16(ry)};
        memcpy(pkt + 9, v, 8);
        SendAll(s, pkt, sizeof(pkt));
    }

    // (see SendHomeTap below for the HOME system-button packet)
    // Touch: press / (throttled) move / release in console pixel space (PunchProbe.c ApplyTouchPacket).
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    auto sendTouch = [&](bool pressed, float nx, float ny) {
        int x = (int)(nx * 1280.0f), y = (int)(ny * 720.0f);
        x = x < 0 ? 0 : (x > 1279 ? 1279 : x);
        y = y < 0 ? 0 : (y > 719 ? 719 : y);
        uint8_t pkt[5] = {(uint8_t)(pressed ? 1 : 0), (uint8_t)(x & 0xFF), (uint8_t)(x >> 8), (uint8_t)(y & 0xFF),
                          (uint8_t)(y >> 8)};
        SendAll(s, pkt, sizeof(pkt));
    };
    if (touchActive)
    {
        const float dx = (touchX - g_lastTx) * 1280.0f, dy = (touchY - g_lastTy) * 720.0f;
        if (!g_prevTouch)
        {
            sendTouch(true, touchX, touchY);
            g_lastTx = touchX;
            g_lastTy = touchY;
            g_lastTouchSend = now;
        }
        else if ((dx * dx + dy * dy) >= 64.0f && now - g_lastTouchSend >= 0.016)
        {
            sendTouch(true, touchX, touchY);
            g_lastTx = touchX;
            g_lastTy = touchY;
            g_lastTouchSend = now;
        }
    }
    else if (g_prevTouch)
        sendTouch(false, 0, 0);
    g_prevTouch = touchActive;
    return true;
}

// HOME from the stream menu. 5-byte "system button" packet [0x03][flags bit0 = HOME held][0][0][0]
// (PunchProbe.c SYS_PACKET_TYPE); the console auto-releases a HOME whose release never arrives.
static bool SendSystemButton(bool homeHeld)
{
    if ((State)g_state.load() != State::Connected)
        return false;
    std::lock_guard<std::mutex> lock(g_sendMutex);
    const int s = g_sock.load();
    if (s < 0)
        return false;
    uint8_t pkt[5] = {0x03, (uint8_t)(homeHeld ? 1 : 0), 0, 0, 0};
    SendAll(s, pkt, sizeof(pkt));
    return true;
}

// A normal HOME tap: press now, release ~150 ms later. : no thread any more (a detached
// std::thread per tap was the only new moving part on the HOME path when the NRO quit on HOME); the
// release is sent from Poll(), which the UI calls every frame while connected.
static std::atomic<long long> g_homeReleaseAtMs{0}; // steady-clock ms at which to send the release; 0 = none pending
static long long SteadyMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void SendHomeTap()
{
    Log("HOME: sending press");
    SendSystemButton(true);
    g_homeReleaseAtMs.store(SteadyMs() + 150);
}

static void ServiceHomeRelease()
{
    const long long due = g_homeReleaseAtMs.load();
    if (due != 0 && SteadyMs() >= due)
    {
        g_homeReleaseAtMs.store(0);
        SendSystemButton(false);
        Log("HOME: sent release");
    }
}

void Shutdown()
{
    Disconnect();
}


} // namespace Network::Punch
