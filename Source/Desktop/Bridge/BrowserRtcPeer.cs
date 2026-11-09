using NxRemotePlay.Client.Core;
using NxRemotePlay.Client.Sources;
using SIPSorcery.Net;
using SIPSorceryMedia.Abstractions;
using Concentus;
using Concentus.Enums;
using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using System.Threading.Channels;

namespace NxRemotePlay.Client.Bridge;

internal sealed class BrowserRtcPeer : IAsyncDisposable
{
    const int RtpPayloadLimit = 1200;
    internal readonly RTCPeerConnection Connection;
    readonly NativeMediaQueue media = new(96);
    readonly CancellationTokenSource cancel;
    readonly Task sending;
    RTCDataChannel? home;
    RTCDataChannel? input;
    readonly BridgeInputArbiter? controls;
    readonly string participant;
    readonly Channel<byte[]> inputPackets = Channel.CreateBounded<byte[]>(new BoundedChannelOptions(1)
    {
        FullMode = BoundedChannelFullMode.DropOldest,
        SingleReader = true,
        SingleWriter = false
    });
    readonly Task inputReading;
    ulong grant;
    ulong acknowledgedGrant;
    // a viewer that never answers its grant (a browser tab in the background sends nothing) was revoked by the input
    // watchdog after 500 ms and granted again 100 ms later, forever: 12,040 grant/revoke cycles in one session (input-flow CSV of
    // ), each rewriting the console's controller state and ignoring the native client's input while the grant lasted.
    // Now every unanswered grant doubles the wait before the next automatic grant (1, 2, 4 ... 30 s); an answered one resets it.
    long regrantNotBefore;
    int unansweredGrants;
    internal static long RegrantDelayMs(int unanswered) => unanswered <= 0 ? 0 : Math.Min(30000L, 1000L << Math.Min(unanswered - 1, 5));
    internal bool MayGrantAutomatically => Environment.TickCount64 >= Volatile.Read(ref regrantNotBefore);
    long lastControlMessage;
    readonly IOpusEncoder opus;
    readonly short[] pcm = new short[1920];
    readonly byte[] encoded = new byte[1275];
    int pcmCount;
    uint jpegSequence;
    readonly CaptureRtpClock videoClock = new(90000), audioClock = new(48000);
    uint nextVideo, nextAudio;
    bool waitingIdr = true;
    bool lastWasHome;
    byte[] sps = StreamInfo.SPS, pps = StreamInfo.PPS;
    // Delivery measurements for web-host-*.csv (see WebFlowLog): written by the sender task, read and reset once a second by the
    // reporter task. Measurement only; nothing here changes what is sent or when.
    readonly object statsGate = new();
    readonly Task reporting;
    long maxQueueTicks, maxInGapTicks, maxSendTicks, lastVideoPublished, videoBytes;
    int videoAus, idrAus, skippedAus, rtpPackets, audioPackets, homeFrames;
    bool sentIdr, sentSkipped;
    long lastBrowserStats;
    internal string Session => participant;
    // The connection failed or closed; the host frees this viewer's place.
    internal bool Ended => cancel.IsCancellationRequested;
    internal bool HasControl => grant != 0 && !cancel.IsCancellationRequested && controls?.GrantedTo(participant) == true;

    internal BrowserRtcPeer(RTCConfiguration configuration, CancellationToken lifetime, BridgeInputArbiter? controls = null, string participant = "", int bindPort = 0)
    {
        if (bindPort < 0 || bindPort > 65535 || bindPort % 2 != 0) throw new ArgumentException("WebRTC UDP port must be even, or zero for automatic selection.");
        this.controls = controls; this.participant = participant;
        cancel = CancellationTokenSource.CreateLinkedTokenSource(lifetime);
        Connection = new RTCPeerConnection(configuration, bindPort: bindPort);
        Connection.addTrack(new MediaStreamTrack(new VideoFormat(VideoCodecsEnum.H264, 96, 90000,
            "packetization-mode=1;profile-level-id=640c20;level-asymmetry-allowed=1"), MediaStreamStatusEnum.SendOnly));
        Connection.addTrack(new MediaStreamTrack(new AudioFormat(111, "opus", 48000, 2, "minptime=10;useinbandfec=1;stereo=1"), MediaStreamStatusEnum.SendOnly));
        opus = OpusCodecFactory.CreateEncoder(48000, 2, OpusApplication.OPUS_APPLICATION_RESTRICTED_LOWDELAY);
        opus.Bitrate = 128000;
        Connection.ondatachannel += channel =>
        {
            if (channel.label == "home")
            {
                home = channel;
                // The page reports its own receive-side numbers here once a second ("w1,<numbers>"); an older page sends nothing.
                channel.onmessage += (dc, protocol, bytes) =>
                {
                    long now = Environment.TickCount64;
                    if (bytes is null || bytes.Length > WebFlowLog.MaxBrowserMessage || now - Interlocked.Read(ref lastBrowserStats) < 400 || cancel.IsCancellationRequested) return;
                    Interlocked.Exchange(ref lastBrowserStats, now);
                    WebFlowLog.Browser(participant, bytes);
                };
            }
            else if (channel.label == "input" && controls is not null && participant.Length != 0)
            {
                if (input is not null) return;
                input = channel;
                channel.onmessage += (dc, protocol, bytes) =>
                {
                    if (bytes.Length == 48 && !cancel.IsCancellationRequested) inputPackets.Writer.TryWrite((byte[])bytes.Clone());
                };
            }
        };
        Connection.onconnectionstatechange += state => { if (state is RTCPeerConnectionState.failed or RTCPeerConnectionState.closed) cancel.Cancel(); };
        sending = Task.Run(Send);
        inputReading = Task.Run(ReadInput);
        reporting = Task.Run(Report);
    }
    async Task Report()
    {
        double ms = 1000.0 / Stopwatch.Frequency;
        try
        {
            while (!cancel.IsCancellationRequested)
            {
                await Task.Delay(1000, cancel.Token);
                long queue, inGap, send, bytes; int aus, idr, skipped, rtp, audio, homes;
                lock (statsGate)
                {
                    queue = maxQueueTicks; inGap = maxInGapTicks; send = maxSendTicks; bytes = videoBytes; aus = videoAus; idr = idrAus; skipped = skippedAus; rtp = rtpPackets; audio = audioPackets; homes = homeFrames;
                    maxQueueTicks = maxInGapTicks = maxSendTicks = videoBytes = 0; videoAus = idrAus = skippedAus = rtpPackets = audioPackets = homeFrames = 0;
                }
                var state = Connection.connectionState;
                if (state != RTCPeerConnectionState.connected) continue;
                WebFlowLog.Host(participant, string.Create(CultureInfo.InvariantCulture,
                    $"{state},{media.Pending},{queue * ms:0.0},{inGap * ms:0.0},{send * ms:0.0},{aus},{idr},{skipped},{bytes},{rtp},{audio},{homes},{home?.bufferedAmount ?? 0}"));
            }
        }
        catch (OperationCanceledException) when (cancel.IsCancellationRequested) { }
        catch (Exception) { }
    }
    // One sent (or skipped) media packet: how long it waited in this viewer's queue, how long handing it to the network took, and
    // (video) the gap since the previous video frame reached the queue.
    void NoteSent(long publishedAt, long startedAt, bool audio, bool homeFrame, int length)
    {
        long done = Stopwatch.GetTimestamp();
        lock (statsGate)
        {
            maxQueueTicks = Math.Max(maxQueueTicks, startedAt - publishedAt);
            maxSendTicks = Math.Max(maxSendTicks, done - startedAt);
            if (homeFrame) { homeFrames++; return; }
            if (audio) { audioPackets++; return; }
            if (lastVideoPublished != 0) maxInGapTicks = Math.Max(maxInGapTicks, publishedAt - lastVideoPublished);
            lastVideoPublished = publishedAt;
            if (sentSkipped) skippedAus++; else { videoAus++; videoBytes += length; if (sentIdr) idrAus++; }
        }
    }
    internal async Task GrantControl()
    {
        if (controls is null || input?.readyState != RTCDataChannelState.open || cancel.IsCancellationRequested)
            throw new InvalidOperationException("Browser controller is unavailable.");
        grant = await controls.Grant(participant);
        acknowledgedGrant = 0;
        if (cancel.IsCancellationRequested) { await controls.Revoke(participant); throw new IOException("Browser disconnected during handoff."); }
        input.send("control:" + grant.ToString(System.Globalization.CultureInfo.InvariantCulture));
        lastControlMessage = Environment.TickCount64;
    }
    async Task ReadInput()
    {
        try
        {
            while (!cancel.IsCancellationRequested)
            {
                while (inputPackets.Reader.TryRead(out var packet))
                    if (controls is not null && await controls.RemoteSnapshot(participant, packet))
                    {
                        acknowledgedGrant = BinaryPrimitives.ReadUInt64LittleEndian(packet.AsSpan(8));
                        if (acknowledgedGrant == grant) { unansweredGrants = 0; Volatile.Write(ref regrantNotBefore, 0); }
                    }
                if (grant != 0 && controls?.GrantedTo(participant) != true)
                {
                    if (acknowledgedGrant != grant) { unansweredGrants++; Volatile.Write(ref regrantNotBefore, Environment.TickCount64 + RegrantDelayMs(unansweredGrants)); }
                    grant = 0; if (input?.readyState == RTCDataChannelState.open) input.send("control:0");
                }
                // This channel deliberately drops stale input rather than retransmitting it.
                // Repeat its automatic-control notification until a valid snapshot arrives.
                if (grant != 0 && acknowledgedGrant != grant && input?.readyState == RTCDataChannelState.open && Environment.TickCount64 - lastControlMessage >= 150)
                {
                    input.send("control:" + grant.ToString(System.Globalization.CultureInfo.InvariantCulture));
                    lastControlMessage = Environment.TickCount64;
                }
                await Task.Delay(20, cancel.Token);
            }
        }
        catch (OperationCanceledException) when (cancel.IsCancellationRequested) { }
        catch (Exception) { cancel.Cancel(); }
        finally { if (controls is not null) try { await controls.Revoke(participant); } catch (Exception) { } }
    }
    internal void Publish(ReceivedPacket packet)
    {
        if (cancel.IsCancellationRequested || Connection.connectionState != RTCPeerConnectionState.connected || packet.Buffer is null) return;
        if (!media.TryPublish(packet)) cancel.Cancel();
    }
    async Task Send()
    {
        try
        {
            await foreach (var entry in media.ReadTimed(cancel.Token))
            {
                var packet = entry.Packet;
                long started = Stopwatch.GetTimestamp();
                bool audio = false, homeFrame = false; int length = 0;
                sentIdr = sentSkipped = false;
                try
                {
                    if (packet.Header.IsError) continue;
                    length = packet.Buffer?.Span.Length ?? 0;
                    if (packet.Header.IsScreenshot) { homeFrame = true; SendHome(packet.Buffer.Span); }
                    else if (packet.Header.IsAudio) { audio = true; SendAudio(packet.Buffer.Span, packet.Header.Timestamp); }
                    else SendVideo(packet.Buffer.Span, packet.Header.Timestamp);
                    NoteSent(entry.PublishedAt, started, audio, homeFrame, length);
                }
                finally { packet.Buffer?.Free(); }
            }
        }
        catch (OperationCanceledException) { }
        finally { Connection.Close("media session ended"); }
    }
    void SendHome(ReadOnlySpan<byte> jpeg)
    {
        waitingIdr = true;
        if (!lastWasHome) pcmCount = 0; // Do not join an old game's partial audio block to resumed capture.
        lastWasHome = true;
        var channel = home;
        if (channel is null || channel.readyState != RTCDataChannelState.open || channel.bufferedAmount > 65536) return;
        uint sequence = ++jpegSequence;
        for (int offset = 0; offset < jpeg.Length; offset += 12000)
        {
            int length = Math.Min(12000, jpeg.Length - offset);
            var chunk = new byte[12 + length];
            BinaryPrimitives.WriteUInt32LittleEndian(chunk, sequence);
            BinaryPrimitives.WriteUInt32LittleEndian(chunk.AsSpan(4), (uint)jpeg.Length);
            BinaryPrimitives.WriteUInt32LittleEndian(chunk.AsSpan(8), (uint)offset);
            jpeg.Slice(offset, length).CopyTo(chunk.AsSpan(12));
            channel.send(chunk, 0, chunk.Length);
        }
    }
    void SendAudio(ReadOnlySpan<byte> audio, ulong timestamp)
    {
        if (audio.Length % 4 != 0) throw new IOException("Invalid stereo PCM packet.");
        uint packetTime = audioClock.At(timestamp, nextAudio);
        // A partially assembled Opus block starts before this PCM packet.
        uint blockTime = unchecked(packetTime - (uint)(pcmCount / 2));
        if (unchecked((int)(blockTime - nextAudio)) >= 0) nextAudio = blockTime;
        for (int at = 0; at < audio.Length; at += 2)
        {
            pcm[pcmCount++] = BinaryPrimitives.ReadInt16LittleEndian(audio.Slice(at, 2));
            if (pcmCount == pcm.Length)
            {
                int count = opus.Encode(pcm.AsSpan(), 960, encoded.AsSpan(), encoded.Length);
                Connection.SendRtpRaw(SDPMediaTypesEnum.audio, encoded.AsSpan(0, count).ToArray(), nextAudio, 0,
                    Connection.AudioStream.GetSendingFormat().ID);
                nextAudio = unchecked(nextAudio + 960); pcmCount = 0;
            }
        }
    }
    internal static List<(int Offset, int Length, int Type)> Nals(ReadOnlySpan<byte> bytes)
    {
        var result = new List<(int, int, int)>(); int start = -1, prefix = 0;
        for (int at = 0; at + 3 <= bytes.Length; at++)
        {
            int width = bytes[at] == 0 && bytes[at + 1] == 0 ? (bytes[at + 2] == 1 ? 3 : at + 3 < bytes.Length && bytes[at + 2] == 0 && bytes[at + 3] == 1 ? 4 : 0) : 0;
            if (width == 0) continue;
            if (start >= 0 && at > start + prefix) result.Add((start, at - start, bytes[start + prefix] & 31));
            start = at; prefix = width; at += width - 1;
        }
        if (start >= 0 && bytes.Length > start + prefix) result.Add((start, bytes.Length - start, bytes[start + prefix] & 31));
        return result;
    }
    void SendVideo(ReadOnlySpan<byte> video, ulong timestamp)
    {
        bool idr = false, hasSps = false, hasPps = false;
        foreach (var nal in Nals(video))
        {
            if (nal.Type == 7) { sps = video.Slice(nal.Offset, nal.Length).ToArray(); hasSps = true; }
            if (nal.Type == 8) { pps = video.Slice(nal.Offset, nal.Length).ToArray(); hasPps = true; }
            if (nal.Type == 5) idr = true;
        }
        if (waitingIdr && !idr) { sentSkipped = true; return; }
        sentIdr = idr;
        byte[] accessUnit;
        if (idr && (!hasSps || !hasPps))
        {
            int prefix = (hasSps ? 0 : sps.Length) + (hasPps ? 0 : pps.Length);
            accessUnit = new byte[prefix + video.Length]; int at = 0;
            if (!hasSps) { sps.CopyTo(accessUnit, at); at += sps.Length; }
            if (!hasPps) { pps.CopyTo(accessUnit, at); at += pps.Length; }
            video.CopyTo(accessUnit.AsSpan(at));
        }
        else accessUnit = video.ToArray();
        waitingIdr = false;
        uint rawRtp = videoClock.At(timestamp, nextVideo);
        nextVideo = unchecked(rawRtp + 1500);
        if (lastWasHome && home?.readyState == RTCDataChannelState.open) { home.send("game"); lastWasHome = false; }
        SendVideoRtp(accessUnit, rawRtp);
    }
    void SendVideoRtp(byte[] accessUnit, uint timestamp)
    {
        int payloadType = Connection.VideoStream.GetSendingFormat().ID;
        // Reuse SIPSorcery's H264 NAL parsing/FU-A headers and encrypted RTP sender,
        // while providing the capture timestamp through its supported raw RTP API.
        int sentRtp = 0;
        try
        {
            foreach (var nal in H264Packetiser.ParseNals(accessUnit))
            {
                if (nal.NAL.Length <= RtpPayloadLimit)
                {
                    sentRtp++;
                    Connection.SendRtpRaw(SDPMediaTypesEnum.video, nal.NAL, timestamp, nal.IsLast ? 1 : 0, payloadType);
                }
                else
                {
                    for (int offset = 1; offset < nal.NAL.Length; offset += RtpPayloadLimit - 2)
                    {
                        int length = Math.Min(RtpPayloadLimit - 2, nal.NAL.Length - offset);
                        bool last = offset + length == nal.NAL.Length;
                        sentRtp++;
                        byte[] prefix = H264Packetiser.GetH264RtpHeader(nal.NAL[0], offset == 1, last);
                        byte[] payload = new byte[prefix.Length + length]; prefix.CopyTo(payload, 0);
                        nal.NAL.AsSpan(offset, length).CopyTo(payload.AsSpan(prefix.Length));
                        Connection.SendRtpRaw(SDPMediaTypesEnum.video, payload, timestamp, nal.IsLast && last ? 1 : 0, payloadType);
                    }
                }
            }
        }
        finally { lock (statsGate) rtpPackets += sentRtp; }
    }
    public async ValueTask DisposeAsync()
    {
        cancel.Cancel(); Connection.Close("bridge viewer left");
        try { await Task.WhenAll(sending, inputReading, reporting); } finally { media.Dispose(); (opus as IDisposable)?.Dispose(); Connection.Dispose(); cancel.Dispose(); }
    }
}
