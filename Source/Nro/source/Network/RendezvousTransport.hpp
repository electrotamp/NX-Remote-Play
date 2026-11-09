#pragma once
#include <cstdint>

// UDP primitive backing Network::Rendezvous (see RendezvousClient.hpp for the REGISTER/CONNECT
// protocol built on top of it), implemented with libnx BSD sockets in PlatformSwitch.cpp.
namespace Network::Transport
{
// Returns -1 if this platform failed to create a socket.
int CreateUdpSocket();

// Returns true if the whole datagram was accepted for sending. host must
// be a literal dotted-decimal IPv4 address - no DNS resolution here,
// matching the same limitation already present in the sysmodule's own
// SocketTcpConnect/RegisterProbe.
bool SendTo(int socket, const char *host, uint16_t port, const void *data, int size);

// Non-blocking. Returns the number of bytes received (0 = nothing
// pending right now), or -1 on a real error.
int RecvFrom(int socket, void *buffer, int maxSize);
} // namespace Network::Transport
