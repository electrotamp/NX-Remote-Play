#include "UdpVideoSession.hpp"
#include "../HostProtocol/udp_nat.h"
#include <algorithm>
#include <cstring>
#include <utility>

namespace Network::Punch
{
uint32_t ReadLe32(const uint8_t *p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint64_t ReadLe64(const uint8_t *p)
{
    return ReadLe32(p) | uint64_t(ReadLe32(p + 4)) << 32;
}
static void Write32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        p[i] = uint8_t(v >> (8 * i));
}
void BuildStreamRequest(uint8_t out[16], bool rendezvous, const uint8_t *ticket)
{
    const uint8_t request[16] = {0xaa, 0xaa, 0xaa, 0xaa, '0', '3', 3, 2, 1, 0};
    memcpy(out, request, 16);
    out[9] = 0x10 | 0x20 | (rendezvous ? 0x40 : 0);
    if (ticket)
        memcpy(out + 10, ticket, 6);
}
static uint16_t NetworkPort(uint16_t port)
{
    // Horizon and local test targets are little endian; avoid socket dependencies.
    uint8_t bytes[2] = {uint8_t(port >> 8), uint8_t(port)};
    uint16_t out;
    memcpy(&out, bytes, 2);
    return out;
}
static bool ValidWireHeader(const uint8_t *p)
{
    return ReadLe32(p) == 0xcccccccc && ReadLe32(p + 4) > 0 && ReadLe32(p + 4) <= MaxWirePayload;
}
bool WireStream::Feed(const uint8_t *data, size_t size, const WireCallback &onWire)
{
    // Feed at most one header/body remainder at a time: memory is bounded even if
    // the caller supplies arbitrarily many concatenated packets in one read.
    while (size)
    {
        const size_t goal = pending.size() < 18 ? 18 : 18 + ReadLe32(pending.data() + 4);
        const size_t take = std::min(size, goal - pending.size());
        pending.insert(pending.end(), data, data + take);
        data += take;
        size -= take;
        if (pending.size() < 18)
            continue;
        if (!ValidWireHeader(pending.data()))
            return false;
        if (pending.size() == 18 + ReadLe32(pending.data() + 4))
        {
            onWire(pending.data(), pending.size());
            pending.clear();
        }
    }
    return true;
}
void UdpVideoSession::AddCandidate(UdpEndpoint e)
{
    if (!e.Valid())
        return;
    for (const auto &c : candidates)
        if (c == e)
            return;
    for (auto &c : candidates)
        if (!c.Valid())
        {
            c = e;
            return;
        }
    // Keep the TCP-derived LAN/direct candidate; refresh the discovered endpoint.
    candidates.back() = e;
}
void UdpVideoSession::SendCandidates(const uint8_t *p, size_t n)
{
    if (locked)
    {
        send(p, n, peer);
        return;
    }
    for (const auto &c : candidates)
        if (c.Valid())
            send(p, n, c);
}
bool UdpVideoSession::Start(const uint8_t response[72], UdpEndpoint tcpPeer, UdpEndpoint server, bool lan, uint64_t now,
                            DatagramSend sendFn, WireCallback onWire)
{
    Stop();
    if (ReadLe32(response) != 6 || ReadLe32(response + 4) != DeliveryAck || !(response[15] & 0x80))
        return false;
    const uint32_t session = ReadLe32(response + 8);
    const uint16_t port = uint16_t(ReadLe32(response + 12));
    if (!session || !port || !tcpPeer.ip)
        return false;
    if (!UdpmReceiverInit(&rx, UdpmType_Video, session, &OnFrame, this))
        return false;
    if (!UdpmReceiverInit(&rxAudio, UdpmType_Audio, session, &OnAudioFrame, this))
    {
        UdpmReceiverFree(&rx);
        return false;
    }
    active = true;
    send = std::move(sendFn);
    deliver = std::move(onWire);
    relay = server;
    nat = relay.Valid() && ReadLe32(response + 32) == UN_MAGIC;
    if (nat)
        memcpy(key.data(), response + 16, 16);
    // A NAT-PMP external port is not necessarily the console's LAN listen port.
    AddCandidate({tcpPeer.ip, NetworkPort(lan ? 9951 : port)});
    AddCandidate({tcpPeer.ip, NetworkPort(port)});
    lastHeard = now;
    nextHello = nextRegister = nextFeedback = nextKey = 0;
    feedbackSeq = 0;
    held.clear();
    schedule = {};
    congestion = {};
    registrations = nacks = audioNacks = keyRequests = unexpected = 0;
    return true;
}
// An audio access unit is exactly one original NxRemotePlay audio record.
void UdpVideoSession::OnAudioFrame(void *ctx, uint8_t, uint32_t, const uint8_t *p, uint32_t n, uint64_t, uint8_t)
{
    auto &self = *static_cast<UdpVideoSession *>(ctx);
    if (n >= 18 && ValidWireHeader(p) && n == 18 + ReadLe32(p + 4) && (p[16] & 3) == 2)
        self.deliver(p, n);
}
void UdpVideoSession::OnFrame(void *ctx, uint8_t, uint32_t, const uint8_t *p, uint32_t n, uint64_t timestamp, uint8_t)
{
    auto &self = *static_cast<UdpVideoSession *>(ctx);
    // An access unit must contain exactly one original NxRemotePlay video record.
    if (n < 18 || !ValidWireHeader(p) || n != 18 + ReadLe32(p + 4) || (p[16] & 3) != 1)
        return;
    const int64_t now = int64_t(self.clock);
    int64_t due = now;
    if (timestamp)
    {
        self.congestion.OnFrame(int64_t(timestamp), now);
        due = self.schedule.PresentAt(int64_t(timestamp), now);
    }
    if (due <= now && self.held.empty())
    {
        self.deliver(p, n);
        return;
    }
    self.held.push_back({due, std::vector<uint8_t>(p, p + n)});
    // The schedule waits at most 80 ms (about five frames); never let the queue grow beyond that.
    while (self.held.size() > 16)
    {
        self.deliver(self.held.front().wire.data(), self.held.front().wire.size());
        self.held.pop_front();
    }
}
void UdpVideoSession::ReleaseDue(uint64_t now)
{
    while (!held.empty() && held.front().due <= int64_t(now))
    {
        // Moved out first: deliver may stop the session, which clears the queue.
        auto wire = std::move(held.front().wire);
        held.pop_front();
        deliver(wire.data(), wire.size());
        if (!active)
            return;
    }
}
bool UdpVideoSession::Push(const uint8_t *p, size_t n, UdpEndpoint from, uint64_t now)
{
    if (!active)
        return false;
    if (nat && from == relay)
    {
        if (!locked && n == 36 && p[4] == 3 && !p[34] && !p[35] && UnMatch(p, n, rx.session, key.data()) && p[28] < 224)
        {
            UdpEndpoint e{};
            memcpy(&e.ip, p + 28, 4);
            memcpy(&e.port, p + 32, 2);
            AddCandidate(e);
            nextHello = 0;
        }
        return false;
    }
    bool known = locked ? from == peer : false;
    if (!locked)
        for (const auto &c : candidates)
            if (c.Valid() && c == from)
                known = true;
    if (!known)
    {
        ++unexpected;
        return false;
    }
    if (nat && n == 28 && p[4] == 4 && UnMatch(p, n, rx.session, key.data()))
    {
        uint8_t hello[UDPM_HELLO_SIZE];
        UdpmWriteHello(hello, rx.session, feedbackSeq);
        send(hello, sizeof(hello), from);
        return false; // proof is not media; do not pin the endpoint yet
    }
    UdpmShardHeader h{};
    if (!UdpmReadShardHeader(p, n, &h) || (h.type != UdpmType_Video && h.type != UdpmType_Audio) ||
        h.session != rx.session || h.frame_size < 19 || h.frame_size > MaxWirePayload + 18)
        return false;
    clock = now;
    if (!UdpmReceiverPush(h.type == UdpmType_Audio ? &rxAudio : &rx, p, n, now))
        return false;
    if (!locked)
    {
        peer = from;
        locked = true;
    }
    lastHeard = now;
    return true;
}
void UdpVideoSession::Poll(uint64_t now)
{
    if (!active)
        return;
    uint8_t buf[UDPM_FEEDBACK_SIZE];
    if (nat && !locked && now >= nextRegister)
    {
        UnWrite(buf, 2, rx.session, key.data());
        send(buf, 28, relay);
        ++registrations;
        UnWrite(buf, 4, rx.session, key.data());
        SendCandidates(buf, 28);
        nextRegister = now + 250000;
    }
    if (now >= nextHello)
    {
        UdpmWriteHello(buf, rx.session, feedbackSeq);
        SendCandidates(buf, UDPM_HELLO_SIZE);
        nextHello = now + (locked ? 1000000 : 250000);
    }
    clock = now;
    UdpmReceiverPoll(&rx, now);
    ReleaseDue(now);
    if (!active)
        return;
    UdpmNack pending[32];
    const int count = UdpmReceiverCollectNacks(&rx, now, pending, 32);
    for (int i = 0; i < count; ++i)
    {
        UdpmWriteNack(buf, rx.session, &pending[i]);
        SendCandidates(buf, UDPM_NACK_SIZE);
        ++nacks;
    }
    UdpmReceiverPoll(&rxAudio, now);
    const int audioCount = UdpmReceiverCollectNacks(&rxAudio, now, pending, 32);
    for (int i = 0; i < audioCount; ++i)
    {
        UdpmWriteNack(buf, rx.session, &pending[i]);
        SendCandidates(buf, UDPM_NACK_SIZE);
        ++audioNacks;
    }
    if (locked && UdpmReceiverNeedsKeyframe(&rx))
        RequestKeyframe(now);
    if (locked && now >= nextFeedback)
    {
        const auto &s = rx.stats;
        const uint32_t expected = s.seq_seen ? s.highest_seq - s.first_seq + 1 : 0;
        UdpmFeedback f{feedbackSeq++,
                       rx.next_id,
                       s.packets_received,
                       expected > s.packets_received ? expected - s.packets_received : 0,
                       s.shards_recovered_fec,
                       s.frames_dropped,
                       s.jitter_us,
                       uint32_t(schedule.TargetDelayUs())};
        UdpmWriteFeedback(buf, rx.session, &f);
        SendCandidates(buf, UDPM_FEEDBACK_SIZE);
        nextFeedback = now + 250000;
    }
    UdpmCongestion report{};
    if (locked && congestion.Report(int64_t(now), nacks, 0, report))
    {
        // The NRO has no audio-underrun counter to share; the host also reacts to the delay fields alone.
        UdpmWriteCongestion(buf, rx.session, &report);
        SendCandidates(buf, UDPM_CONGESTION_SIZE);
    }
}
void UdpVideoSession::RequestKeyframe(uint64_t now)
{
    if (!active || !locked || now < nextKey)
        return;
    uint8_t buf[UDPM_KEYFRAME_REQUEST_SIZE];
    UdpmWriteKeyframeRequest(buf, rx.session, rx.next_id - 1);
    SendCandidates(buf, sizeof(buf));
    ++keyRequests;
    nextKey = now + 250000;
}
void UdpVideoSession::Stop()
{
    UdpmReceiverFree(&rx);
    UdpmReceiverFree(&rxAudio);
    held.clear();
    active = nat = locked = false;
    candidates = {};
    peer = {};
    relay = {};
    key = {};
    send = {};
    deliver = {};
}
void DeliveryProgress::Packet(const uint8_t *p, size_t size, uint64_t now)
{
    if (size < 18)
        return;
    bytes += size;
    if ((p[16] & 3) != 1 || (p[16] & 0x28))
        return;
    const uint64_t timestamp = ReadLe64(p + 8);
    if (timestamp)
        echo = timestamp;
    if (lastVideo && now >= lastVideo)
        maxGap = std::max(maxGap, uint32_t(std::min<uint64_t>(UINT32_MAX, now - lastVideo)));
    lastVideo = now;
    ++videos;
}
bool DeliveryProgress::Report(uint64_t now, uint8_t out[32])
{
    if (!echo || (lastReport && now - lastReport < 250000))
        return false;
    memset(out, 0, 32);
    out[0] = 0x7e;
    out[1] = 1;
    out[2] = 32;
    if (!++sequence)
        ++sequence;
    Write32(out + 4, sequence);
    Write32(out + 8, uint32_t(echo));
    Write32(out + 12, uint32_t(echo >> 32));
    Write32(out + 16, videos);
    Write32(out + 20, uint32_t(std::min<uint64_t>(UINT32_MAX, bytes)));
    Write32(out + 24, maxGap);
    bytes = 0;
    maxGap = 0;
    lastReport = now;
    return true;
}
} // namespace Network::Punch
