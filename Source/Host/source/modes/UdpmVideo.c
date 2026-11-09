#include "UdpmVideo.h"
#include "modes.h"
#include "../core.h"
#include "../net/sockets.h"
#include "../udpm/udpm_sender.h"
#include "../udpm/udpm_fec.h"
#include "../nxrp_video.h"
#include "../net/udpm_control.h"
#include "../net/udp_nat.h"
#include <string.h>
#include <poll.h>
#include <stdatomic.h>

#ifndef NX_EAGAIN
#define NX_EAGAIN 11 // same value net/sockets.c uses (Horizon BSD errno)
#endif

#define RTX_SLOTS 128                // ~184 KB: NACK window of ~115 KB of video plus the audio shards
#define SILENT_LIMIT_US 5000000ULL
// Pacing: P-frames at 2x the 10 Mbps target; keyframes faster so an IDR does not take
// ~80 ms to leave ( phone probe: that burst became ~100 ms of playout delay).
#define PACE_NORMAL_BPS 20000000u
#define PACE_KEYFRAME_BPS 45000000u
// A client outside the local network sits behind a router uplink and a cellular or home queue. Measured on cellular:
// each NACK named about 11 missing datagrams (one whole P frame) and FEC never rebuilt anything, so frames
// are lost whole. Smaller bursts and lower peak rates give those queues time to drain. 18 Mbps measured worse than 12
// on the same cellular path, so 12 stays until a repeat run separates pace from path variance.
#define PACE_WAN_NORMAL_BPS 12000000u
#define PACE_WAN_KEYFRAME_BPS 24000000u
#define BURST_LAN_BYTES (16 * 1024)
#define BURST_WAN_BYTES (6 * 1024)

static UdpmSender g_tx;
static UdpmRtxSlot g_rtx[RTX_SLOTS];
static atomic_bool g_reserved;
static int g_sock = -1;
static u32 g_session;
static bool g_bound, g_nat, g_natPeer, g_txReady, g_lan;
static u8 g_natKey[16];
static struct sockaddr_in g_relay, g_candidate;
static u32 g_natSent, g_natRelayRx;
static u32 g_bindDatagrams, g_helloWrongIp; // this bind window only, for the failure line
void UdpmVideo_NatOffer(u8 key[16])
{
    randomGet(g_natKey, sizeof(g_natKey));
    memcpy(key, g_natKey, 16);
    g_nat = true;
}
static struct sockaddr_in g_peer;
static u64 g_lastHeardUs, g_lastReportUs;
static u32 g_eagain, g_dropped, g_hellos;
static u64 g_lastFrameUs, g_frameGapUs, g_frameSendUs, g_frameAgeUs;
static u64 g_audioUs, g_waitUs, g_drainUs, g_reportUs;
static UdpmFeedback g_fb;
static UqControl g_control;
static u32 g_sourceEpoch;
static atomic_uint g_targetBps = UQ_CEILING_BPS;
static u32 g_maxBsdUs;

// Per-window timing histograms, diagnostics only (the 5 s support lines). Fed from the writer thread (send), the capture
// thread (delivery wait, capture) and read by the service thread, so relaxed atomics.
#define HIST_BUCKETS 7
static const u32 kDurationLimitsMs[HIST_BUCKETS - 1] = {2, 5, 10, 17, 34, 67};
static const u32 kGapLimitsMs[HIST_BUCKETS - 1] = {12, 20, 25, 34, 50, 100};
static atomic_uint g_hSend[HIST_BUCKETS], g_hWait[HIST_BUCKETS], g_hSendGap[HIST_BUCKETS], g_hCaptureGap[HIST_BUCKETS];
static atomic_uint g_captures, g_skipFrames, g_skipPresents;
static atomic_ullong g_lastCaptureUs;
// Audio health for the window: frames sent, silent PCM frames, loudest sample, longest gap between audio sends.
static atomic_uint g_audioFrames, g_audioSilent, g_audioPeak, g_audioGapMaxUs;
static atomic_ullong g_lastAudioUs;

static void HistAdd(atomic_uint *hist, const u32 *limitsMs, u64 us)
{
    unsigned i = 0;
    while (i < HIST_BUCKETS - 1 && us >= (u64)limitsMs[i] * 1000ULL)
        i++;
    atomic_fetch_add_explicit(&hist[i], 1, memory_order_relaxed);
}

static void HistTake(atomic_uint *hist, u32 out[HIST_BUCKETS])
{
    for (unsigned i = 0; i < HIST_BUCKETS; i++)
        out[i] = atomic_exchange_explicit(&hist[i], 0, memory_order_relaxed);
}

u32 UdpmVideo_TargetBps(void)
{
    return atomic_load(&g_targetBps);
}

static u64 NowUs(void *c)
{
    (void)c;
    return armTicksToNs(armGetSystemTick()) / 1000;
}
static void SleepUs(void *c, u64 us)
{
    (void)c;
    svcSleepThread(us * 1000ULL);
}

static int SendDg(void *c, const uint8_t *d, size_t len)
{
    (void)c;
    const u64 t0 = NowUs(NULL);
    for (;;)
    {
        const u64 callStart = NowUs(NULL);
        const ssize_t n = bsdSendTo(g_sock, d, len, MSG_DONTWAIT, (struct sockaddr *)&g_peer, sizeof(g_peer));
        const int err = g_bsdErrno;
        const u32 callUs = (u32)(NowUs(NULL) - callStart);
        if (callUs > g_maxBsdUs)
            g_maxBsdUs = callUs;
        if (n == (ssize_t)len)
        {
            break;
        }
        if (n < 0 && err == NX_EAGAIN && NowUs(NULL) - t0 < 20000)
        {
            g_eagain++;
            struct pollfd p = {.fd = g_sock, .events = POLLOUT, .revents = 0};
            bsdPoll(&p, 1, 2);
            continue;
        }
        if (n < 0 && err != NX_EAGAIN)
        {
            return -1;
        }
        g_dropped++; // buffer stayed full for 20 ms: count as loss, FEC/NACK cover it
        break;
    }
    return 0;
}

static bool OpenSocket(void)
{
    if (g_sock >= 0)
        return true;
    const int s = SocketUdp();
    if (s < 0)
        return false;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(UDPM_VIDEO_PORT);
    a.sin_addr.s_addr = INADDR_ANY;
    if (bsdBind(s, (struct sockaddr *)&a, sizeof(a)) < 0 || !SocketMakeNonBlocking(s))
    {
        bsdClose(s);
        return false;
    }
    g_sock = s;
    return true;
}

u64 UdpmVideo_Offer(void)
{
    bool expected = false;
    if (!atomic_compare_exchange_strong(&g_reserved, &expected, true))
        return 0; // one UDP viewer at a time
    if (!OpenSocket())
    {
        atomic_store(&g_reserved, false);
        return 0;
    }
    randomGet(&g_session, sizeof(g_session));
    if (!g_session)
        g_session = 1;
    g_bound = g_nat = g_natPeer = g_txReady = false;
    return UDPM_OFFER_BIT | ((u64)UDPM_VIDEO_PORT << 32) | g_session;
}

void UdpmVideo_End(void)
{
    if (!atomic_load(&g_reserved))
        return;
    g_bound = false;
    g_session = 0;
    g_nat = g_natPeer = g_txReady = false;
    memset(g_natKey, 0, sizeof(g_natKey));
    atomic_store(&g_reserved, false);
}

// Reads every pending datagram. Before binding only a Hello from the TCP peer's IP counts.
static void Drain(u32 tcpPeerIp)
{
    u8 rx[64];
    for (unsigned budget = 0; budget < 128; ++budget)
    {
        struct sockaddr_in from = {0};
        socklen_t fl = sizeof(from);
        const ssize_t r = bsdRecvFrom(g_sock, rx, sizeof(rx), MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
        if (r <= 0)
            return;
        g_bindDatagrams++;
        if (g_nat && UnMatch(rx, (size_t)r, g_session, g_natKey))
        {
            if (r == 36 && rx[4] == 3 && !rx[34] && !rx[35] && from.sin_addr.s_addr == g_relay.sin_addr.s_addr &&
                from.sin_port == g_relay.sin_port)
            {
                memset(&g_candidate, 0, sizeof(g_candidate));
                g_candidate.sin_family = AF_INET;
                memcpy(&g_candidate.sin_addr.s_addr, rx + 28, 4);
                memcpy(&g_candidate.sin_port, rx + 32, 2);
                g_natPeer = g_candidate.sin_port && g_candidate.sin_addr.s_addr && (rx[28] < 224);
                ++g_natRelayRx;
            }
            else if (r == 28 && rx[4] == 4 && g_natPeer && from.sin_addr.s_addr == g_candidate.sin_addr.s_addr &&
                     from.sin_port == g_candidate.sin_port)
            {
                g_peer = from;
                g_bound = true;
                g_lastHeardUs = NowUs(NULL);
            }
            continue;
        }
        const int type = UdpmPeekType(rx, (size_t)r);
        u32 v;
        UdpmNack nk;
        UdpmCongestion cg;
        if (type == UdpmType_Hello && UdpmReadHello(rx, (size_t)r, g_session, &v))
        {
            if (!g_bound)
            {
                if (from.sin_addr.s_addr != tcpPeerIp)
                {
                    g_helloWrongIp++;
                    continue; // token alone is not enough
                }
                g_peer = from;
                g_bound = true;
            }
            else if (from.sin_addr.s_addr == g_peer.sin_addr.s_addr)
            {
                g_peer.sin_port = from.sin_port; // NAT may move the client's port
            }
            else
                continue;
            g_hellos++;
            g_lastHeardUs = NowUs(NULL);
            continue;
        }
        if (!g_bound || !g_txReady || from.sin_addr.s_addr != g_peer.sin_addr.s_addr ||
            from.sin_port != g_peer.sin_port)
            continue;
        if (type == UdpmType_Nack && UdpmReadNack(rx, (size_t)r, g_session, &nk))
        {
            UdpmSenderHandleNack(&g_tx, &nk);
        }
        else if (type == UdpmType_KeyframeRequest && UdpmReadKeyframeRequest(rx, (size_t)r, g_session, &v))
        {
            NxrpVideo_RequestKeyframe();
        }
        else if (type == UdpmType_Congestion && UdpmReadCongestion(rx, (size_t)r, g_session, &cg))
        {
            const u32 epoch = NxrpVideo_SourceEpoch();
            if (epoch != g_sourceEpoch)
            {
                g_sourceEpoch = epoch;
                UqQuiet(&g_control, NowUs(NULL));
            }
            if (!UqApplyCongestion(&g_control, cg.seq, cg.interval_ms, cg.frames, cg.excess_min_us, cg.excess_max_us,
                                   cg.audio_underruns, NowUs(NULL)))
                continue;
            atomic_store(&g_targetBps, g_control.target_bps);
        }
        else if (type == UdpmType_Feedback)
        {
            UdpmFeedback fb;
            if (!UdpmReadFeedback(rx, (size_t)r, g_session, &fb) ||
                !UqApply(&g_control, fb.seq, fb.frames_dropped, NowUs(NULL)))
                continue;
            g_fb = fb;
            atomic_store(&g_targetBps, g_control.target_bps);
        }
        else
            continue;
        g_lastHeardUs = NowUs(NULL);
    }
}

bool UdpmVideo_Bind(int tcpSocket, u32 waitMs, const char *relayIp, u16 relayPort)
{
    if (!atomic_load(&g_reserved) || g_sock < 0)
        return false;
    struct sockaddr_in tcpPeer = {0};
    socklen_t tl = sizeof(tcpPeer);
    if (bsdGetPeerName(tcpSocket, (struct sockaddr *)&tcpPeer, &tl) < 0)
    {
        return false;
    }
    UqReset(&g_control);
    atomic_store(&g_targetBps, UQ_CEILING_BPS);
    UdpmSenderConfig cfg;
    UdpmSenderDefaultConfig(&cfg);
    memset(&g_relay, 0, sizeof(g_relay));
    g_relay.sin_family = AF_INET;
    g_relay.sin_port = htons(relayPort);
    g_relay.sin_addr.s_addr = SocketResolveHostV4(relayIp);
    g_nat = g_nat && relayPort && g_relay.sin_addr.s_addr != INADDR_NONE;
    // A client on the same network sends its Hello straight to this console; only a remote
    // client needs the longer NAT-assisted window.
    const u32 peerIp = ntohl(tcpPeer.sin_addr.s_addr);
    const bool lan = (peerIp >> 24) == 10 || (peerIp >> 20) == 0xAC1 || (peerIp >> 16) == 0xC0A8;
    g_lan = lan;
    cfg.burst_bytes = lan ? BURST_LAN_BYTES : BURST_WAN_BYTES;
    if (g_nat && !lan)
        waitMs = 8000;
    u64 nextRegister = 0;
    g_natSent = g_natRelayRx = 0;
    g_bindDatagrams = g_helloWrongIp = g_hellos = 0;
    const u64 start = NowUs(NULL);
    while (!g_bound && NowUs(NULL) - start < (u64)waitMs * 1000ULL)
    {
        const u64 now = NowUs(NULL);
        if (g_nat && now >= nextRegister)
        {
            u8 packet[28];
            UnWrite(packet, 1, g_session, g_natKey);
            g_natSent++;
            bsdSendTo(g_sock, packet, sizeof(packet), MSG_DONTWAIT, (struct sockaddr *)&g_relay, sizeof(g_relay));
            if (g_natPeer)
            {
                UnWrite(packet, 4, g_session, g_natKey);
                bsdSendTo(g_sock, packet, sizeof(packet), MSG_DONTWAIT, (struct sockaddr *)&g_candidate,
                          sizeof(g_candidate));
            }
            nextRegister = now + 250000;
        }
        Drain(tcpPeer.sin_addr.s_addr);
        if (!g_bound)
            svcSleepThread(5000000ULL);
    }
    if (!g_bound)
    {
        // Release builds keep this line so a failed bind names its cause.
        NetworkSupportLine("UDP bind failed lan=%u waited_ms=%u datagrams=%u hellos=%u hello_wrong_ip=%u nat_regs=%u relay_replies=%u",
                           lan ? 1u : 0u, waitMs, g_bindDatagrams, g_hellos, g_helloWrongIp, g_natSent, g_natRelayRx);
        // Open a fresh socket for the next offer in case this one stopped receiving.
        bsdClose(g_sock);
        g_sock = -1;
        return false;
    }
    NetworkSupportLine("UDP bound lan=%u after_ms=%u hellos=%u pace=%u/%u Mbps burst=%u B bsd_sessions=%u",
                       lan ? 1u : 0u, (u32)((NowUs(NULL) - start) / 1000), g_hellos,
                       (lan ? PACE_NORMAL_BPS : PACE_WAN_NORMAL_BPS) / 1000000u,
                       (lan ? PACE_KEYFRAME_BPS : PACE_WAN_KEYFRAME_BPS) / 1000000u, cfg.burst_bytes,
                       SocketBsdSessions());
    UdpmSenderInit(&g_tx, &cfg, g_session, g_rtx, RTX_SLOTS, SendDg, NowUs, SleepUs, NULL);
    g_txReady = true;
    g_eagain = g_dropped = g_hellos = 0;
    memset(&g_fb, 0, sizeof(g_fb));
    g_maxBsdUs = 0;
    g_lastHeardUs = g_lastReportUs = NowUs(NULL);
    g_lastFrameUs = g_frameGapUs = g_frameSendUs = g_frameAgeUs = 0;
    atomic_store(&g_lastCaptureUs, 0);
    g_audioUs = g_waitUs = g_drainUs = g_reportUs = 0;
    return true;
}

// Keyframe = an IDR slice, SPS (the hub injects SPS/PPS before IDRs) or a JPEG still.
static bool IsKeyframe(const u8 *wire, u32 size)
{
    if (size < 18)
        return false;
    if (wire[16] & BIT(6))
        return true; // PacketMeta_Content_Screenshot: independently decodable
    const u8 *p = wire + 18;
    const u32 n = size - 18 < 512 ? size - 18 : 512;
    for (u32 i = 0; i + 3 < n; i++)
    {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1)
        {
            const u8 t = p[i + 3] & 0x1F;
            if (t == 5 || t == 7)
                return true;
            if (t == 1)
                return false; // a non-IDR slice came first
        }
    }
    return false;
}

// One audio wire packet as a UDPM audio frame (same socket, clock and writer thread as the video, own frame ids, FEC
// and NACK retransmission). Small frames are a single data shard with the configured parity, so a lost datagram is
// rebuilt at once and a burst is covered by the parity copies.
bool UdpmVideo_SendAudio(const u8 *wire, u32 size)
{
    const u64 now = NowUs(NULL);
    const u64 last = atomic_exchange_explicit(&g_lastAudioUs, now, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_audioFrames, 1, memory_order_relaxed);
    if (last && now > last && now - last > atomic_load_explicit(&g_audioGapMaxUs, memory_order_relaxed))
        atomic_store_explicit(&g_audioGapMaxUs, (u32)(now - last), memory_order_relaxed);
    return UdpmSenderSendFrame(&g_tx, UdpmType_Audio, wire, size, now, 0) >= 0;
}

// One 20 ms PCM frame (16-bit little-endian stereo) about to be Opus-encoded for a UDP viewer.
void UdpmVideo_NoteAudioPcm(const u8 *pcm, u32 size)
{
    if (!g_bound)
        return;
    u32 peak = 0;
    for (u32 i = 0; i + 1 < size; i += 2)
    {
        const int sample = (s16)(pcm[i] | (pcm[i + 1] << 8));
        const u32 magnitude = sample < 0 ? (u32)-sample : (u32)sample;
        if (magnitude > peak)
            peak = magnitude;
    }
    if (peak == 0)
        atomic_fetch_add_explicit(&g_audioSilent, 1, memory_order_relaxed);
    if (peak > atomic_load_explicit(&g_audioPeak, memory_order_relaxed))
        atomic_store_explicit(&g_audioPeak, peak, memory_order_relaxed);
}

bool UdpmVideo_Send(const u8 *wire, u32 size, u64 *pacedNs)
{
    const u64 begin = NowUs(NULL);
    if (g_lastFrameUs)
    {
        HistAdd(g_hSendGap, kGapLimitsMs, begin - g_lastFrameUs);
        if (begin - g_lastFrameUs > g_frameGapUs)
            g_frameGapUs = begin - g_lastFrameUs;
    }
    g_lastFrameUs = begin;
    // This path carries the tap's local monotonic capture timestamp.
    if (size >= 18)
    {
        u64 captured;
        memcpy(&captured, wire + 8, sizeof(captured));
        if (captured && begin >= captured && begin - captured > g_frameAgeUs)
            g_frameAgeUs = begin - captured;
    }
    const bool key = IsKeyframe(wire, size);
    if (g_lan)
        UdpmSenderSetPace(&g_tx, key ? PACE_KEYFRAME_BPS : PACE_NORMAL_BPS);
    else
        UdpmSenderSetPace(&g_tx, key ? PACE_WAN_KEYFRAME_BPS : PACE_WAN_NORMAL_BPS);
    const u64 waitBefore = g_tx.stats.paced_wait_us;
    const int64_t id = UdpmSenderSendFrame(&g_tx, UdpmType_Video, wire, size, NowUs(NULL), key ? UdpmFlag_Keyframe : 0);
    *pacedNs = (g_tx.stats.paced_wait_us - waitBefore) * 1000ULL;
    const u64 elapsed = NowUs(NULL) - begin;
    HistAdd(g_hSend, kDurationLimitsMs, elapsed);
    if (elapsed > g_frameSendUs)
        g_frameSendUs = elapsed;
    return id >= 0;
}

// The capture loop waited this long for the previous frame's send to finish.
void UdpmVideo_NoteDeliveryWait(u64 us)
{
    if (g_bound)
        HistAdd(g_hWait, kDurationLimitsMs, us);
}

// One game frame was captured; skippedPresents is how many presents the game made since the previous capture.
void UdpmVideo_NoteCapture(u32 skippedPresents)
{
    if (!g_bound)
        return;
    const u64 now = NowUs(NULL);
    const u64 last = atomic_exchange_explicit(&g_lastCaptureUs, now, memory_order_relaxed);
    if (last && now > last)
        HistAdd(g_hCaptureGap, kGapLimitsMs, now - last);
    atomic_fetch_add_explicit(&g_captures, 1, memory_order_relaxed);
    if (skippedPresents)
    {
        atomic_fetch_add_explicit(&g_skipFrames, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_skipPresents, skippedPresents, memory_order_relaxed);
    }
}

void UdpmVideo_RecordLoopTime(u64 audioUs, u64 waitUs)
{
    if (!g_bound)
        return;
    if (audioUs > g_audioUs)
        g_audioUs = audioUs;
    if (waitUs > g_waitUs)
        g_waitUs = waitUs;
}

void UdpmVideo_Service(void)
{
    if (!g_bound)
        return;
    const u64 drainBegin = NowUs(NULL);
    Drain(g_peer.sin_addr.s_addr);
    const u64 now = NowUs(NULL);
    if (now - drainBegin > g_drainUs)
        g_drainUs = now - drainBegin;
    if (now - g_lastReportUs >= 5000000ULL)
    {
        const UdpmSenderStats *s = &g_tx.stats;
        // nanoprintf: plain %u only.
        const u64 reportBegin = NowUs(NULL);
        // Bounded support summaries remain available in release builds.
        NetworkSupportLine("UDP flow window_ms=%u frames=%u bytes=%u rtx=%u eagain=%u send_drop=%u bsd_us=%u paced_ms=%u",
                           (u32)((now - g_lastReportUs) / 1000), s->frames, (u32)s->bytes, s->retransmits,
                           g_eagain, g_dropped, g_maxBsdUs, (u32)(s->paced_wait_us / 1000));
        NetworkSupportLine("UDP timing capture_to_send_us=%u send_us=%u send_gap_us=%u fb_age_ms=%u",
                           (u32)g_frameAgeUs, (u32)g_frameSendUs, (u32)g_frameGapUs,
                           g_control.seen ? (u32)((now - g_control.last_us) / 1000) : UINT32_MAX);
        NetworkSupportLine("UDP feedback target_bps=%u cuts=%u recoveries=%u dropped=%u jitter_us=%u playout_us=%u cong_reports=%u cong_cuts=%u",
                           g_control.target_bps, g_control.reductions, g_control.recoveries,
                           g_fb.frames_dropped, g_fb.jitter_us, g_fb.playout_delay_us, g_control.cong_reports,
                           g_control.cong_cuts);
        NetworkSupportLine("UDP loop audio_us=%u wait_us=%u drain_us=%u previous_report_us=%u",
                           (u32)g_audioUs, (u32)g_waitUs, (u32)g_drainUs, (u32)g_reportUs);
        u32 send[HIST_BUCKETS], wait[HIST_BUCKETS], sendGap[HIST_BUCKETS], captureGap[HIST_BUCKETS];
        HistTake(g_hSend, send);
        HistTake(g_hWait, wait);
        HistTake(g_hSendGap, sendGap);
        HistTake(g_hCaptureGap, captureGap);
        // Bucket edges in ms; the last bucket is everything above the final edge.
        NetworkSupportLine("UDP hist <2/5/10/17/34/67/more ms: send %u %u %u %u %u %u %u wait %u %u %u %u %u %u %u",
                           send[0], send[1], send[2], send[3], send[4], send[5], send[6], wait[0], wait[1], wait[2],
                           wait[3], wait[4], wait[5], wait[6]);
        NetworkSupportLine("UDP hist <12/20/25/34/50/100/more ms: send_gap %u %u %u %u %u %u %u capture_gap %u %u %u %u %u %u %u captures=%u skip_frames=%u skip_presents=%u",
                           sendGap[0], sendGap[1], sendGap[2], sendGap[3], sendGap[4], sendGap[5], sendGap[6],
                           captureGap[0], captureGap[1], captureGap[2], captureGap[3], captureGap[4], captureGap[5],
                           captureGap[6], atomic_exchange_explicit(&g_captures, 0, memory_order_relaxed),
                           atomic_exchange_explicit(&g_skipFrames, 0, memory_order_relaxed),
                           atomic_exchange_explicit(&g_skipPresents, 0, memory_order_relaxed));
        NetworkSupportLine("UDP audio frames=%u silent=%u peak=%u max_gap_ms=%u",
                           atomic_exchange_explicit(&g_audioFrames, 0, memory_order_relaxed),
                           atomic_exchange_explicit(&g_audioSilent, 0, memory_order_relaxed),
                           atomic_exchange_explicit(&g_audioPeak, 0, memory_order_relaxed),
                           atomic_exchange_explicit(&g_audioGapMaxUs, 0, memory_order_relaxed) / 1000);
        g_reportUs = NowUs(NULL) - reportBegin;
        g_audioUs = g_waitUs = g_drainUs = 0;
        g_frameGapUs = g_frameSendUs = g_frameAgeUs = 0;
        g_maxBsdUs = 0;
        memset(&g_tx.stats, 0, sizeof(g_tx.stats));
        g_eagain = g_dropped = g_hellos = 0;
        g_lastReportUs = now;
    }
}

bool UdpmVideo_Alive(void)
{
    return g_bound && NowUs(NULL) - g_lastHeardUs < SILENT_LIMIT_US;
}
