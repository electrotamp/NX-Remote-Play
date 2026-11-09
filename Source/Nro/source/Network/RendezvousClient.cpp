#include "RendezvousClient.hpp"
#include "RendezvousTransport.hpp"
#include "PunchConnector.hpp"
#include "../Platform/fs.hpp"
#include "../HostProtocol/server_config.h"

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <chrono>
#include <sstream>

// Switch playback uses the TCP/UDPM connector.
namespace Network::Rendezvous
{
namespace
{

std::string serverHost;
uint16_t serverPort = 0;
bool configLoaded = false;

// background Rendezvous-server reachability indicator (footer
// status, SceneHost.cpp's Footer()) - added directly because of a real
// support cycle where "Connect" appeared to silently do nothing and took a
// long manual log-diving session to trace to the PC's Rendezvous.exe simply
// not running. A dedicated socket, independent of Connect()/PollHandshake()'s
// own socketHandle, so a reachability probe reply can never be confused with
// a real CONNECT/PEER exchange in flight. Uses the server's existing REFRESH
// command with a credential that can never be a real session as a free,
// side-effect-free ping - HandleRefresh (RendezvousServer.cs) always replies
// (OK or ERR NOTFOUND) even for an unknown id, so any reply at all proves the
// server process is up and reachable; no reply within the deadline means it
// isn't. Runs continuously once configured, independent of whether the user
// is even on the Connect screen, so the indicator is accurate the moment you
// look at it rather than only after attempting a real connect.
int reachabilitySocket = -1;
std::chrono::steady_clock::time_point nextReachabilityPingSend;
std::chrono::steady_clock::time_point reachabilityReplyDeadline;
bool waitingForReachabilityReply = false;
bool serverReachable = false;

void PollServerReachability()
{
    if (!configLoaded)
        return;
    if (reachabilitySocket < 0)
    {
        reachabilitySocket = Transport::CreateUdpSocket();
        if (reachabilitySocket < 0)
            return;
    }

    auto now = std::chrono::steady_clock::now();
    if (!waitingForReachabilityReply && now >= nextReachabilityPingSend)
    {
        static const char *ping = "REFRESH __reachability_probe__ __reachability_probe__";
        Transport::SendTo(reachabilitySocket, serverHost.c_str(), serverPort, ping, (int)strlen(ping));
        waitingForReachabilityReply = true;
        reachabilityReplyDeadline = now + std::chrono::milliseconds(2000);
    }

    if (waitingForReachabilityReply)
    {
        char buf[64];
        int received = Transport::RecvFrom(reachabilitySocket, buf, sizeof(buf) - 1);
        if (received > 0)
        {
            serverReachable = true;
            waitingForReachabilityReply = false;
            nextReachabilityPingSend =
                now + std::chrono::milliseconds(
                          10000); // was 3 s - probes share the server's per-IP rate limit with real traffic
        }
        else if (now >= reachabilityReplyDeadline)
        {
            serverReachable = false;
            waitingForReachabilityReply = false;
            nextReachabilityPingSend =
                now + std::chrono::milliseconds(
                          10000); // was 3 s - probes share the server's per-IP rate limit with real traffic
        }
    }
}

} // namespace

bool IsServerReachable()
{
    return serverReachable;
}

bool LoadServerConfig()
{
    configLoaded = false;

    // The shared server file (same file, same parser as the sysmodule) wins; without one the built-in default applies.
    ServerConfig cfg;
    ServerCfg_Defaults(&cfg);
    const std::string sharedPath = std::string(SDMC) + SERVERCFG_PATH;
    if (fs::Exists(sharedPath))
    {
        try
        {
            auto bytes = fs::OpenFile(sharedPath);
            std::string text(bytes.begin(), bytes.end());
            ServerCfg_Parse(text.c_str(), &cfg);
        }
        catch (...)
        {
        }
    }
    char h[SERVERCFG_TEXT_MAX];
    unsigned pt = 0;
    if (!ServerCfg_Split(ServerCfg_Effective(&cfg), h, sizeof(h), &pt))
        return false;
    serverHost = h;
    serverPort = (uint16_t)pt;
    configLoaded = true;
    return true;
}

// Points the footer's reachability probe at a new server right away (the NRO's Server page
// calls this after the sysmodule accepts a new server) and restarts the probe.
bool GetServerConfig(std::string &host, uint16_t &port)
{
    if (!configLoaded || serverHost.empty() || serverPort == 0)
        return false;
    host = serverHost;
    port = serverPort;
    return true;
}

void SetProbeServer(const std::string &host, uint16_t port)
{
    serverHost = host;
    serverPort = port;
    configLoaded = true;
    serverReachable = false;
    waitingForReachabilityReply = false;
    nextReachabilityPingSend = std::chrono::steady_clock::now();
}

// Connector facade for Switch playback.
namespace Connector
{

void Connect(const std::string &id, const std::string &password)
{
    // Pair with the configured server through the TCP hole-punch connector.
    // (An unconfigured server is an empty host, which Punch::Connect reports as a bad address.)
    Punch::Connect(configLoaded ? serverHost : std::string(), serverPort, id, password);
    return;
}

void Poll()
{
    // Runs unconditionally, independent of connect state, so the footer's
    // reachability indicator (SceneHost.cpp's Footer()) stays accurate no
    // matter which screen is showing.
    PollServerReachability();

    Punch::Poll();
    return;
}

// Punch::State -> the public Rendezvous::State (same meanings; Registered is never used).
State GetState()
{
    switch (Punch::GetState())
    {
    case Punch::State::Requesting:
        return State::Requesting;
    case Punch::State::Paired:
        return State::Paired;
    case Punch::State::Handshaking:
        return State::Handshaking;
    case Punch::State::Connected:
        return State::Connected;
    case Punch::State::Error:
        return State::Error;
    default:
        return State::Idle;
    }
}
const std::string &GetErrorMessage()
{
    return Punch::GetErrorMessage();
}
void SetOnVideoFrame(void (*callback)(uint8_t *data, size_t size, void *userdata), void *userdata)
{
    Punch::SetOnVideoFrame(callback, userdata);
}
void SetOnStillFrame(void (*callback)(uint8_t *data, size_t size, void *userdata), void *userdata)
{
    Punch::SetOnStillFrame(callback, userdata);
}
void Disconnect()
{
    Punch::Disconnect();
}
std::string GetStatusText()
{
    return Punch::GetStatusText();
}
void Note(const std::string &line)
{
    Punch::Note(line);
}

bool SendInputState(bool touchActive, float touchX, float touchY, uint64_t buttons, int32_t lx, int32_t ly, int32_t rx,
                    int32_t ry)
{
    return Punch::SendInputState(touchActive, touchX, touchY, buttons, lx, ly, rx, ry);
}

void SendHomeTap()
{
    Punch::SendHomeTap();
}

// Stop the active connector before the application releases its resources.
void Shutdown()
{
    Punch::Shutdown();
}

} // namespace Connector

} // namespace Network::Rendezvous
