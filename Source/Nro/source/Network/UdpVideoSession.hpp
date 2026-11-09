#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>
#include "../udpm/udpm_receiver.h"
#include "Playout.hpp"

namespace Network::Punch
{
// Endpoint fields retain socket/network byte order. No sockets or threads here:
// the existing connector worker owns both transports and invokes this state machine.
struct UdpEndpoint
{
    uint32_t ip = 0;
    uint16_t port = 0;
    bool operator==(const UdpEndpoint &b) const
    {
        return ip == b.ip && port == b.port;
    }
    bool Valid() const
    {
        return ip && port;
    }
};
using WireCallback = std::function<void(const uint8_t *, size_t)>;
using DatagramSend = std::function<void(const uint8_t *, size_t, UdpEndpoint)>;

uint32_t ReadLe32(const uint8_t *p);
uint64_t ReadLe64(const uint8_t *p);
constexpr uint32_t DeliveryAck = 0x3346514e;
constexpr size_t MaxWirePayload = 0x54000;
void BuildStreamRequest(uint8_t out[16], bool rendezvous, const uint8_t *ticket);

// Incremental TCP reader: partial audio must never block servicing UDP video.
class WireStream
{
    std::vector<uint8_t> pending;

  public:
    bool Feed(const uint8_t *data, size_t size, const WireCallback &onWire);
};

class UdpVideoSession
{
    UdpmReceiver rx{};
    UdpmReceiver rxAudio{}; // audio frames of the same UDPM stream: own ids, FEC and NACKs, no keyframe gating
    std::array<UdpEndpoint, 3> candidates{};
    UdpEndpoint relay{}, peer{};
    std::array<uint8_t, 16> key{};
    DatagramSend send;
    WireCallback deliver;
    bool active = false, nat = false, locked = false;
    uint64_t lastHeard = 0, nextHello = 0, nextRegister = 0, nextFeedback = 0, nextKey = 0;
    uint32_t feedbackSeq = 0;
    // Complete video frames wait here until their presentation time, so bursty arrival leaves evenly spaced.
    struct HeldFrame
    {
        int64_t due;
        std::vector<uint8_t> wire;
    };
    std::deque<HeldFrame> held;
    PresentationSchedule schedule;
    CongestionMeter congestion;
    uint64_t clock = 0; // time of the Push/Poll that is delivering frames
    void ReleaseDue(uint64_t now);
    static void OnFrame(void *, uint8_t, uint32_t, const uint8_t *, uint32_t, uint64_t, uint8_t);
    static void OnAudioFrame(void *, uint8_t, uint32_t, const uint8_t *, uint32_t, uint64_t, uint8_t);
    void AddCandidate(UdpEndpoint endpoint);
    void SendCandidates(const uint8_t *, size_t);

  public:
    uint32_t registrations = 0, nacks = 0, audioNacks = 0, keyRequests = 0, unexpected = 0;
    UdpVideoSession() = default;
    ~UdpVideoSession()
    {
        Stop();
    }
    UdpVideoSession(const UdpVideoSession &) = delete;
    UdpVideoSession &operator=(const UdpVideoSession &) = delete;
    // Ports passed here are network order. offer's advertised port is converted locally.
    bool Start(const uint8_t response[72], UdpEndpoint tcpPeer, UdpEndpoint server, bool lan, uint64_t now,
               DatagramSend sendFn, WireCallback onWire);
    bool Push(const uint8_t *, size_t, UdpEndpoint from, uint64_t now);
    void Poll(uint64_t now);
    void RequestKeyframe(uint64_t now);
    void Stop();
    bool Active() const
    {
        return active;
    }
    bool Locked() const
    {
        return locked;
    }
    bool Silent(uint64_t now) const
    {
        return active && now - lastHeard >= 15000000;
    }
    const UdpmRxStats &Stats() const
    {
        return rx.stats;
    }
    int64_t PlayoutDelayUs() const
    {
        return schedule.TargetDelayUs();
    }
    std::string CongestionSummary()
    {
        return congestion.Summary();
    }
};

// Reports wire-read completions, never physical presentation or cross-clock latency.
class DeliveryProgress
{
    uint64_t echo = 0, lastVideo = 0, lastReport = 0, bytes = 0;
    uint32_t sequence = 0, videos = 0, maxGap = 0;

  public:
    void Packet(const uint8_t *wire, size_t size, uint64_t now);
    bool Report(uint64_t now, uint8_t out[32]);
};
} // namespace Network::Punch
