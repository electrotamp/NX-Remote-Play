#pragma once
#include <switch.h>
#include <arpa/inet.h> // htons
#include <switch/services/bsd.h>

#define SOCKET_INVALID -1

void SocketInit();

// How many bsd sessions SocketInit() got (0 before it has run).
u32 SocketBsdSessions(void);

void SocketDeinit();

int SocketUdp();

// Recreates the UDP debug-log socket (no-op unless built with UDP_LOGGING).

// Literal IPv4 or hostname (UDP DNS, cached); network byte order, INADDR_NONE on failure. Blocking, bounded.
u32 SocketResolveHostV4(const char *host);

// Returns a nonblocking socket after connecting to a hostname or IPv4 address.

// Bind the signaling connection to the local port used for hole punching.
int SocketTcpConnectFromPort(const char *ip, u16 port, u16 localPort);

typedef enum
{
    // Accept succeeded, guaranteed to have a valid socket in outAccepted
    SocketAcceptError_OK,
    // Accept failed, the console was put into sleep mode and the listener needs to be reset
    SocketAcceptError_NetDown,
    // Accept failed, either no pending connections or an error occured, try again
    SocketAcceptError_Fail,
} SocketAcceptResult;

SocketAcceptResult SocketTcpAccept(int listenerHandle, int *outAccepted, struct sockaddr *addr, socklen_t *addrlen);

// Closes a socket and resets its handle to SOCKET_INVALID
void SocketClose(int *socket);
// Control writes have a five-second deadline and do not require a legacy stream mode.
bool SocketSendControlAll(int socket, const void *buffer, u32 size);
// Dedicated media writer; never calls the input hook. The separate input owner
// remains responsive while this writer waits for POLLOUT. Cancellation is bounded.
typedef struct
{
    u32 (*reserve)(void *, u32);
    void (*refund)(void *, u32, u32);
} SocketMediaBudget;
bool SocketSendMediaPaced(int socket, const void *buffer, u32 size, bool (*keepSending)(void *), void *context,
                          const SocketMediaBudget *budget, u64 *pacingNs);
// optional. Called by SocketSendAll when the socket turns readable while a send is
// backed up (client->console bytes such as touch/controller packets, or a peer FIN). Return
// false if the peer is gone. Lets input be serviced during a long frame send.
extern bool (*SocketSendReadableHook)(int socket);

// Returns true on success
bool SocketUDPSendTo(int socket, const void *data, u32 size, struct sockaddr *addr, socklen_t addrlen);

// Returns true on success

// Accounts for the network mask

// Returns 0 for EAGAIN, -1 on peer close or error, otherwise bytes received.
s32 SocketRecv(int socket, void *buffer, u32 size);
// Immediate nonblocking poll, without handshake retry yielding on EAGAIN.
s32 SocketTryRecv(int socket, void *buffer, u32 size);

bool SocketMakeNonBlocking(int socket);

int SocketNativeErrno();
