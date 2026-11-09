// Included inside PunchConnector's private namespace; shares its one network worker.
uint64_t NowUs()
{
    return uint64_t(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
UdpEndpoint Endpoint(const sockaddr_in &a)
{
    return {a.sin_addr.s_addr, a.sin_port};
}
sockaddr_in SocketAddress(UdpEndpoint e)
{
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = e.ip;
    a.sin_port = e.port;
    return a;
}

void StreamLoop(int s, const uint8_t response[72], const sockaddr_in &server, bool lan)
{
    // No extra Switch network worker: bounded nonblocking reads keep partial TCP
    // audio, UDP recovery and cancellation serviced by this same loop.
    SetTimeouts(s, 250);
    uint32_t frames = 0, audioPackets = 0;
    uint64_t lastVideo = 0, lastAny = NowUs(), nextLog = lastAny + 5000000;
    uint64_t maxGap = 0;
    uint32_t gaps50 = 0, gaps150 = 0;
    DeliveryProgress progress;
    WireStream tcp;
    UdpVideoSession udp;
    bool protocolError = false, firstUdp = false;
    int udpSocket = -1;
    const bool feedback = ReadLe32(response + 4) == DeliveryAck;
    auto wire = [&](const uint8_t *p, size_t n, bool fromUdp) {
        const uint8_t meta = p[16];
        const auto now = NowUs();
        lastAny = now;
        progress.Packet(p, n, now);
        if (meta & 0x20)
        {
            Log("Video: host error packet");
            return;
        }
        if ((meta & 3) == 2)
        {
            NroAudio::Push(p + 18, n - 18);
            ++audioPackets;
            return;
        }
        if (meta & 0x08)
            return;
        if ((meta & 3) != 1)
            return;
        if (!fromUdp && udp.Active())
        {
            // Fallback guard: unused offered UDP cannot time out a working TCP stream.
            if (udp.Locked())
            {
                Log("Video: unexpected TCP video after UDP bound");
                protocolError = true;
                return;
            }
            Log("NXRP N001: host selected TCP video fallback");
            udp.Stop();
            close(udpSocket);
            udpSocket = -1;
        }
        if (fromUdp && !firstUdp)
        {
            firstUdp = true;
            Log("UDPM NRO: first complete video packet received");
        }
        if (lastVideo)
        {
            const auto gap = now - lastVideo;
            maxGap = std::max(maxGap, gap);
            if (gap > 50000)
                ++gaps50;
            if (gap > 150000)
                ++gaps150;
        }
        lastVideo = now;
        ++frames;
        PushFrame(std::vector<uint8_t>(p + 18, p + n), (meta & 0x40) != 0);
    };
    sockaddr_in peer{};
    socklen_t peerSize = sizeof(peer);
    if (getpeername(s, (sockaddr *)&peer, &peerSize) == 0 && feedback && (response[15] & 0x80))
    {
        udpSocket = socket(AF_INET, SOCK_DGRAM, 0);
        if (udpSocket >= 0)
        {
            int rcvSize = 1 << 20;
            setsockopt(udpSocket, SOL_SOCKET, SO_RCVBUF, &rcvSize, sizeof(rcvSize));
            const int flags = fcntl(udpSocket, F_GETFL, 0);
            sockaddr_in local{};
            local.sin_family = AF_INET;
            if (flags < 0 || fcntl(udpSocket, F_SETFL, flags | O_NONBLOCK) < 0 ||
                bind(udpSocket, (sockaddr *)&local, sizeof(local)) < 0)
            {
                close(udpSocket);
                udpSocket = -1;
            }
        }
        if (udpSocket >= 0 && !udp.Start(
                                  response, Endpoint(peer), Endpoint(server), lan, NowUs(),
                                  [&](const uint8_t *p, size_t n, UdpEndpoint e) {
                                      const auto a = SocketAddress(e);
                                      sendto(udpSocket, p, n, MSG_DONTWAIT, (const sockaddr *)&a, sizeof(a));
                                  },
                                  [&](const uint8_t *p, size_t n) { wire(p, n, true); }))
        {
            close(udpSocket);
            udpSocket = -1;
        }
        Log(udp.Active() ? "NXRP N001: UDP media offered (video and audio); input remains on TCP"
                         : "NXRP N001: UDP initialization failed; awaiting host TCP fallback");
    }
    else
        Log("NXRP N001: TCP video (no UDP offer)");

    while (!Cancelled() && !protocolError)
    {
        auto now = NowUs();
        udp.Poll(now);
        if (g_requestKeyframe.exchange(false))
            udp.RequestKeyframe(now);
        fd_set reads;
        FD_ZERO(&reads);
        FD_SET(s, &reads);
        if (udpSocket >= 0)
            FD_SET(udpSocket, &reads);
        timeval timeout{0, 2000};
        const int ready = select(std::max(s, udpSocket) + 1, &reads, nullptr, nullptr, &timeout);
        if (ready < 0)
        {
            if (errno == EINTR)
                continue;
            Log("Video: socket select failed");
            break;
        }
        if (udpSocket >= 0 && FD_ISSET(udpSocket, &reads))
        {
            for (unsigned budget = 0; budget < 128 && !Cancelled(); ++budget)
            {
                uint8_t dg[UDPM_MAX_DATAGRAM + 64];
                sockaddr_in from{};
                socklen_t fl = sizeof(from);
                const ssize_t n = recvfrom(udpSocket, dg, sizeof(dg), MSG_DONTWAIT, (sockaddr *)&from, &fl);
                if (n < 0)
                {
                    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                        protocolError = true;
                    break;
                }
                // Zero-length datagrams are ignored, never a stream EOF.
                if (n > 0)
                    udp.Push(dg, size_t(n), Endpoint(from), NowUs());
            }
        }
        if (FD_ISSET(s, &reads))
        {
            for (unsigned budget = 0; budget < 8 && !Cancelled(); ++budget)
            {
                uint8_t chunk[16384];
                const ssize_t n = recv(s, chunk, sizeof(chunk), MSG_DONTWAIT);
                if (n == 0)
                {
                    protocolError = true;
                    break;
                }
                if (n < 0)
                {
                    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                        protocolError = true;
                    break;
                }
                lastAny = NowUs();
                if (!tcp.Feed(chunk, size_t(n), [&](const uint8_t *p, size_t count) { wire(p, count, false); }))
                {
                    Log("Video: malformed TCP packet; closing session");
                    protocolError = true;
                    break;
                }
                if (protocolError)
                    break;
            }
        }
        now = NowUs();
        uint8_t report[32];
        if (feedback && progress.Report(now, report))
        {
            std::lock_guard<std::mutex> lock(g_sendMutex);
            if (!SendAll(s, report, sizeof(report)))
            {
                Log("Video: progress feedback send failed");
                break;
            }
        }
        if (udp.Silent(now))
        {
            Log("UDPM NRO: video silent for 15 seconds");
            break;
        }
        if (!udp.Active() && now - lastAny >= 15000000)
        {
            Log("Video: TCP silent for 15 seconds");
            break;
        }
        if (now >= nextLog)
        {
            Log("N001 receive 5s: video=" + std::to_string(frames) + " audio=" + std::to_string(audioPackets) +
                " gaps>50ms=" + std::to_string(gaps50) + " gaps>150ms=" + std::to_string(gaps150) +
                " maxGapUs=" + std::to_string(maxGap) + " (wire completion, not physical display)");
            if (udp.Active())
            {
                const auto &stats = udp.Stats();
                Log("UDPM NRO 5s: delivered=" + std::to_string(stats.frames_delivered) + " pkts=" +
                    std::to_string(stats.packets_received) + " fec=" + std::to_string(stats.shards_recovered_fec) +
                    " dropped=" + std::to_string(stats.frames_dropped) + " nacks=" + std::to_string(udp.nacks) +
                    " keyreq=" + std::to_string(udp.keyRequests) +
                    " registrations=" + std::to_string(udp.registrations) + " skippedForKeyframe=" +
                std::to_string(stats.frames_skipped_waiting_keyframe) + " packetsLost=" +
                std::to_string(stats.seq_seen ? int64_t(stats.highest_seq - stats.first_seq + 1) -
                                                    int64_t(stats.packets_received)
                                              : 0) +
                    " jitter=" + std::to_string(stats.jitter_us / 1000) + "ms playout=" +
                    std::to_string(udp.PlayoutDelayUs() / 1000) + "ms " + udp.CongestionSummary());
            }
            maxGap = 0;
            gaps50 = gaps150 = 0;
            nextLog = now + 5000000;
        }
    }
    // Every session frees its reassembly state and closes its media socket on this worker.
    udp.Stop();
    if (udpSocket >= 0)
        close(udpSocket);
    Log("N001 stream ended: video=" + std::to_string(frames) + " audio=" + std::to_string(audioPackets));
}
