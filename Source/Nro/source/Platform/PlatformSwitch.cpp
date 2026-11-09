
#include "Platform.hpp"
#include <iostream>
#include <switch.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "../UI/UI.hpp"
#include "../Network/RendezvousTransport.hpp"
#include "../Network/DnsClient.hpp"

GLFWgamepadstate g_gamepad;

void Platform::Init()
{
    romfsInit();
    // Default UDP buffers (~9KB rx) overflow while the main thread is busy decoding, dropping
    // media packets; the UDP receiver needs enough headroom for a burst while decoding.
    SocketInitConfig socketConfig = *socketGetDefaultInitConfig();
    socketConfig.udp_rx_buf_size = 0x40000;
    socketConfig.udp_tx_buf_size = 0x10000;
    // libnx's default TCP receive buffer is 64 KiB (max 256 KiB), smaller than one
    // keyframe (70-90 KB). Over a ~100 ms mobile link a small window makes every keyframe take
    // an extra round trip (measured ~190 ms stalls every 0.5 s, NRO only). Other clients get
    // auto-tuned multi-hundred-KB windows from their OS.
    socketConfig.tcp_rx_buf_size = 0x80000;
    socketConfig.tcp_rx_buf_max_size = 0x100000;
    if (R_FAILED(socketInitialize(&socketConfig)))
        socketInitializeDefault();
    hidInitializeTouchScreen();
}

void Platform::Exit()
{
    socketExit();
    romfsExit();
}

void Platform::GetInputs()
{
    if (!glfwGetGamepadState(GLFW_JOYSTICK_1, &g_gamepad))
        std::cout << "Error reading from gamepad";
}

void Platform::ImguiBindings()
{
    ImGuiIO &io = ImGui::GetIO();
    HidTouchScreenState state;
    if (hidGetTouchScreenStates(&state, 1) && state.count == 1)
    {
        io.MousePos = ImVec2(state.touches[0].x, state.touches[0].y);
        io.MouseDown[0] = true;
    }
    else
        io.MouseDown[0] = false;
}

void Platform::Sleep(float time)
{
    usleep(time * 1000);
}

// Real UDP sockets via libnx's standard BSD sockets layer (socketInitializeDefault(),
// already called in Platform::Init() above) - ordinary POSIX socket()/sendto()/recvfrom(),
// not the sysmodule's low-level bsd:u wrapper (that's a different process with its own
// static-tmem constraints; the NRO doesn't share them).
int Network::Transport::CreateUdpSocket()
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
        return -1;

    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    return sock;
}

bool Network::Transport::SendTo(int socket, const char *host, uint16_t port, const void *data, int size)
{
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(host);
    if (addr.sin_addr.s_addr == INADDR_NONE)
    {
        // A DNS name: resolved without blocking (the first call sends the query, later calls find the answer).
        uint32_t ip = 0;
        if (!Network::Dns::Resolve(host, ip))
            return false;
        addr.sin_addr.s_addr = ip;
    }

    return sendto(socket, data, size, 0, (struct sockaddr *)&addr, sizeof(addr)) == size;
}

int Network::Transport::RecvFrom(int socket, void *buffer, int maxSize)
{
    ssize_t r = recvfrom(socket, buffer, maxSize, 0, nullptr, nullptr);
    if (r < 0)
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    return (int)r;
}

