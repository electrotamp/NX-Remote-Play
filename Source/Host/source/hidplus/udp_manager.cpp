// Most of the UDP code comes from hid-mitm: https://github.com/jakibaki/hid-mitm

#include "udp_manager.hpp"
#include "con_manager.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <stddef.h>
#include <fcntl.h>
extern "C"
{
#include <switch/services/bsd.h>
}

// NxRemotePlay never uses the standard POSIX socket layer (socket()/bind()/close()/recvfrom()
// from newlib) anywhere else in this process - it talks to libnx's "bsd" service directly
// via bsdSocket()/bsdBind()/bsdClose()/bsdRecvFrom() etc (see sockets.c), because
// SocketInit() calls bsdInitialize() itself with a custom config rather than going through
// the standard setup the POSIX layer expects. This file originally used the POSIX layer,
// which turned out to be the actual cause of the "Bad file descriptor" errors on every
// socket() call - mixing the two client-side layers over the same bsd session doesn't
// work. Switched to the same direct bsd* calls the rest of the process already uses.

#define PORT 8000

static int sockfd = -1;

struct sockaddr_in servaddr, cliaddr;

static void setup_socket()
{
    if (sockfd != -1)
        bsdClose(sockfd);

    if ((sockfd = bsdSocket(AF_INET, SOCK_DGRAM, 0)) < 0)
    {
        // This used to be exit(EXIT_FAILURE) - fine in standalone sys-hidplus where this
        // was the whole process, but here it silently killed the entire NxRemotePlay process
        // (video/audio included) over a single retryable socket failure on a background
        // input thread. sockfd stays -1; poll_udp_input()'s own periodic retry logic
        // (which already calls setup_socket() again once curIP/last_time drift) will just
        // try again instead of taking the whole sysmodule down with it.
        char errbuf[160];
        snprintf(errbuf, sizeof(errbuf), "setup_socket: bsdSocket() failed, g_bsdErrno=%d", g_bsdErrno);
        printToFile(errbuf);
        sockfd = -1;
        return;
    }

    // Use the same direct-BSD nonblocking flag as net/sockets.c. Idle input must
    // not block the shared BSD service or recycle a healthy INADDR_ANY socket.
    if (bsdFcntl(sockfd, F_SETFL, 0x800) < 0)
    {
        printToFile("setup_socket: nonblocking configuration failed");
        bsdClose(sockfd);
        sockfd = -1;
        return;
    }

    memset(&servaddr, 0, sizeof(servaddr));
    memset(&cliaddr, 0, sizeof(cliaddr));

    servaddr.sin_family = AF_INET; // IPv4 address
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(PORT);

    if (bsdBind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0)
    {
        char errbuf[160];
        snprintf(errbuf, sizeof(errbuf), "setup_socket: bsdBind() failed, g_bsdErrno=%d", g_bsdErrno);
        printToFile(errbuf);
        bsdClose(sockfd);
        sockfd = -1;
        return;
    }

    printToFile("setup_socket: socket created and bound to port 8000 successfully");
}

static struct input_message cached_message = {0};
static bool have_message = false, attempted = false, polled = false;
static u64 last_attempt, last_poll, last_message;

int poll_udp_input(struct input_message *buf)
{
    const u64 now = svcGetSystemTick();
    if (sockfd < 0 && (!attempted || now - last_attempt >= armNsToTicks(1000000000ULL)))
    {
        attempted = true;
        last_attempt = now;
        setup_socket();
    }
    if (sockfd >= 0 && (!polled || now - last_poll >= armNsToTicks(10000000ULL)))
    {
        polled = true;
        last_poll = now;
        // One extra byte distinguishes oversized datagrams from valid full frames.
        // Four-slot legacy senders remain supported; zero-initialize unused slots.
        u8 bytes[sizeof(struct input_message) + 1] = {0};
        socklen_t len = sizeof(cliaddr);
        int n = bsdRecvFrom(sockfd, bytes, sizeof(bytes), 0, (struct sockaddr *)&cliaddr, &len);
        if (n < 0)
        {
            const int error = g_bsdErrno;
            // Direct BSD uses Linux errno values, as in net/sockets.c.
            if (error != 11 && error != 4) // EAGAIN / EINTR
            {
                bsdClose(sockfd);
                sockfd = -1;
                have_message = false;
                last_attempt = now; // bounded retry even after an immediate failure
            }
        }
        else if (n == (int)sizeof(struct input_message) || n == (int)offsetof(struct input_message, con_type5))
        {
            struct input_message message = {0};
            memcpy(&message, bytes, n);
            unsigned slots = n == (int)sizeof(message) ? 8 : 4;
            if (message.magic == INPUT_MSG_MAGIC && message.con_count <= slots)
            {
                cached_message = message;
                have_message = true;
                last_message = now;
            }
        }
    }
    if (!have_message || now - last_message >= armNsToTicks(1000000000ULL))
        return -1;
    *buf = cached_message;
    return 0;
}
