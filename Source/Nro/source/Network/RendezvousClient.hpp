#pragma once
#include <string>
#include <cstdint>
#include <cstddef>

// Shared server configuration and reachability. Switch connections delegate to the TCP/UDPM connector.
namespace Network::Rendezvous
{
enum class State
{
    Idle,
    Requesting,
    Paired = 3,
    Handshaking,
    Connected,
    Error,
};

bool LoadServerConfig();

void SetProbeServer(const std::string &host, uint16_t port);

bool GetServerConfig(std::string &host, uint16_t &port);

bool IsServerReachable();

namespace Connector
{

void Connect(const std::string &id, const std::string &password);
void Poll();

void Disconnect();

State GetState();
const std::string &GetErrorMessage();

std::string GetStatusText();

void Note(const std::string &line);

// Set callbacks before Connect; they run when the render thread polls the connector.
void SetOnVideoFrame(void (*callback)(uint8_t *data, size_t size, void *userdata), void *userdata);

void SetOnStillFrame(void (*callback)(uint8_t *data, size_t size, void *userdata), void *userdata);

// Forward touch and controller samples only while connected.
bool SendInputState(bool touchActive, float touchX, float touchY, uint64_t buttons, int32_t lx, int32_t ly, int32_t rx,
                    int32_t ry);

void SendHomeTap();

// Stop connector workers before platform teardown. Safe when disconnected.
void Shutdown();
} // namespace Connector
} // namespace Network::Rendezvous
