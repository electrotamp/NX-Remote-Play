using NxRemotePlay.Client.Core;
using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Threading;
using NxRemotePlay.Client.Bridge;

namespace NxRemotePlay.Client.Sources;

// Receiving side of UDPM for the desktop acting as a client of a console. Port of Android's UdpmReceiver.kt /
// UdpmVideoChannel.kt (same rules as Host/source/udpm/udpm_receiver.c): reassembles FEC blocks, rebuilds lost shards,
// delivers frames in order, NACKs what is still missing, and after an unrecoverable video loss withholds P-frames until
// the next keyframe. Not thread-safe: drive each receiver from the single UDP thread.
internal sealed class UdpmStreamReceiver
{
    internal const int Window = 32;
    readonly byte streamType;
    readonly uint session;
    readonly Action<uint, byte[], ulong, byte> onFrame;
    public long HoldUs = 40_000, NackAfterUs = 8_000;

    public int PacketsReceived, ShardsRecoveredFec, FramesDelivered, FramesDropped, FramesSkippedWaitingKeyframe;
    public uint FirstSeq, HighestSeq;
    public bool SeqSeen;
    public long JitterUs;
    public int PacketsExpected => SeqSeen ? (int)(HighestSeq - FirstSeq) + 1 : 0;
    public int PacketsLost => Math.Max(0, PacketsExpected - PacketsReceived);
    public uint NextFrameId => nextId;
    public bool WaitingKeyframe { get; private set; }

    sealed class Block
    {
        public readonly int K, M, ShardSize;
        public long LastUs;
        public readonly byte[] Buf;
        public readonly bool[] Present;
        public int Have, Nacks;
        public bool Done;
        public Block(int k, int m, int shardSize, long lastUs)
        { K = k; M = m; ShardSize = shardSize; LastUs = lastUs; Buf = new byte[(k + m) * shardSize]; Present = new bool[k + m]; }
    }

    sealed class Frame
    {
        public readonly uint Id, Size;
        public readonly int BlockCount;
        public readonly byte Flags;
        public readonly ulong TimestampUs;
        public readonly Block?[] Blocks;
        public int BlocksDone;
        public long CompleteUs;
        public Frame(uint id, uint size, int blockCount, byte flags, ulong timestampUs)
        { Id = id; Size = size; BlockCount = blockCount; Flags = flags; TimestampUs = timestampUs; Blocks = new Block?[blockCount]; }
        public bool Complete => BlocksDone == BlockCount;
    }

    readonly Dictionary<uint, Frame> frames = new();
    uint nextId;
    bool started;
    long transitPrev;
    bool transitInit;

    public UdpmStreamReceiver(byte streamType, uint session, Action<uint, byte[], ulong, byte> onFrame)
    {
        this.streamType = streamType; this.session = session; this.onFrame = onFrame;
        WaitingKeyframe = streamType == UdpmWire.TypeVideo;
    }

    static int Diff(uint a, uint b) => unchecked((int)(a - b));

    // Same validation as UdpmReadShardHeader; false for anything malformed or foreign.
    static bool ParseHeader(ReadOnlySpan<byte> d, out byte type, out byte flags, out uint session, out uint seq, out uint frameId,
        out uint frameSize, out int blockIndex, out int blockCount, out int shardIndex, out int dataShards, out int parityShards,
        out int shardSize, out ulong timestampUs)
    {
        type = flags = 0; session = seq = frameId = frameSize = 0; blockIndex = blockCount = shardIndex = dataShards = parityShards = shardSize = 0; timestampUs = 0;
        if (d.Length < UdpmWire.HeaderSize || d[0] != UdpmWire.Magic || d[1] != UdpmWire.Version) return false;
        type = d[2];
        if (type != UdpmWire.TypeVideo && type != UdpmWire.TypeAudio && type != UdpmWire.TypeProbe) return false;
        flags = d[3];
        session = BinaryPrimitives.ReadUInt32LittleEndian(d[4..]);
        seq = BinaryPrimitives.ReadUInt32LittleEndian(d[8..]);
        frameId = BinaryPrimitives.ReadUInt32LittleEndian(d[12..]);
        frameSize = BinaryPrimitives.ReadUInt32LittleEndian(d[16..]);
        blockIndex = d[20]; blockCount = d[21]; shardIndex = d[22]; dataShards = d[23]; parityShards = d[24];
        shardSize = BinaryPrimitives.ReadUInt16LittleEndian(d[26..]);
        timestampUs = BinaryPrimitives.ReadUInt64LittleEndian(d[28..]);
        if (blockCount == 0 || blockIndex >= blockCount) return false;
        if (dataShards == 0 || dataShards > UdpmWire.MaxDataShards || parityShards > UdpmWire.MaxParityShards) return false;
        if (shardIndex >= dataShards + parityShards || shardSize == 0 || shardSize > UdpmWire.MaxShard) return false;
        if (d.Length != UdpmWire.HeaderSize + shardSize) return false;
        if (((flags & UdpmWire.FlagParity) != 0) != (shardIndex >= dataShards)) return false;
        if (frameSize == 0 || frameSize > (ulong)blockCount * UdpmWire.MaxDataShards * (ulong)shardSize) return false;
        return true;
    }

    public bool Push(ReadOnlySpan<byte> dg, long nowUs)
    {
        if (!ParseHeader(dg, out var type, out var flags, out var hSession, out var seq, out var frameId, out var frameSize, out var blockIndex,
                out var blockCount, out var shardIndex, out var dataShards, out var parityShards, out var shardSize, out var timestampUs))
            return false;
        if (type != streamType || hSession != session) return false;
        if (!started) { nextId = frameId; started = true; }
        // Loss statistics count every valid datagram, including parity that arrives after its frame was already
        // delivered; only exact repeats of a tracked shard are taken back out.
        if (!SeqSeen) { FirstSeq = HighestSeq = seq; SeqSeen = true; }
        else if (Diff(seq, HighestSeq) > 0) HighestSeq = seq;
        PacketsReceived++;
        if (Diff(frameId, nextId) < 0) return false;
        while (Diff(frameId, nextId) >= Window)
        {
            frames.Remove(nextId);
            FramesDropped++;
            if (streamType == UdpmWire.TypeVideo) WaitingKeyframe = true;
            nextId++;
        }
        if (!frames.TryGetValue(frameId, out var f))
        {
            long transit = nowUs - (long)timestampUs;
            if (transitInit) JitterUs += (Math.Abs(transit - transitPrev) - JitterUs) / 16;
            transitPrev = transit; transitInit = true;
            f = new Frame(frameId, frameSize, blockCount, (byte)(flags & UdpmWire.FlagKeyframe), timestampUs);
            frames[frameId] = f;
        }
        if (f.Size != frameSize || f.BlockCount != blockCount) return false;
        var b = f.Blocks[blockIndex];
        if (b is null) { b = new Block(dataShards, parityShards, shardSize, nowUs); f.Blocks[blockIndex] = b; }
        if (b.K != dataShards || b.M != parityShards || b.ShardSize != shardSize) return false;
        if (b.Present[shardIndex]) { PacketsReceived--; return true; }
        b.Present[shardIndex] = true;
        b.LastUs = nowUs;
        if (b.Done) return true;
        dg.Slice(UdpmWire.HeaderSize, shardSize).CopyTo(b.Buf.AsSpan(shardIndex * shardSize, shardSize));
        b.Have++;
        if (b.Have >= b.K)
        {
            int rebuilt = UdpmFec.Decode(b.Buf, b.Present, b.K, b.M, b.ShardSize);
            if (rebuilt >= 0)
            {
                ShardsRecoveredFec += rebuilt;
                b.Done = true;
                if (++f.BlocksDone == f.BlockCount) f.CompleteUs = nowUs;
            }
        }
        Poll(nowUs);
        return true;
    }

    // Delivers or drops frames whose wait expired. Call every few ms even without traffic.
    public void Poll(long nowUs)
    {
        if (!started) return;
        while (true)
        {
            if (frames.TryGetValue(nextId, out var f) && f.Complete)
            {
                Deliver(f);
                frames.Remove(nextId);
                nextId++;
                continue;
            }
            bool expired = false;
            for (uint i = 1; i < Window; i++)
            {
                if (!frames.TryGetValue(nextId + i, out var n)) continue;
                if (n.Complete) { expired = nowUs - n.CompleteUs >= HoldUs; break; }
            }
            if (!expired) return;
            frames.Remove(nextId);
            FramesDropped++;
            if (streamType == UdpmWire.TypeVideo) WaitingKeyframe = true;
            nextId++;
        }
    }

    void Deliver(Frame f)
    {
        bool key = (f.Flags & UdpmWire.FlagKeyframe) != 0;
        if (WaitingKeyframe && !key) { FramesSkippedWaitingKeyframe++; return; }
        WaitingKeyframe = false;
        var output = new byte[f.Size];
        int blockBytes = f.Blocks[0]!.K * f.Blocks[0]!.ShardSize, offset = 0;
        for (int b = 0; b < f.BlockCount; b++)
        {
            int want = (int)Math.Min((uint)blockBytes, f.Size - (uint)(b * blockBytes));
            Buffer.BlockCopy(f.Blocks[b]!.Buf, 0, output, offset, want);
            offset += want;
        }
        FramesDelivered++;
        onFrame(f.Id, output, f.TimestampUs, f.Flags);
    }

    // Blocks still short of k shards after NackAfterUs (twice at most per block).
    public void CollectNacks(long nowUs, List<UdpmWire.Nack> output, int max = 16)
    {
        foreach (var f in frames.Values)
            for (int bi = 0; bi < f.Blocks.Length; bi++)
            {
                if (output.Count >= max) return;
                var b = f.Blocks[bi];
                // Only once the block has gone quiet: a paced block legitimately takes ~20 ms to arrive.
                if (b is null || b.Done || b.Nacks >= 2 || nowUs - b.LastUs < NackAfterUs * (b.Nacks + 1)) continue;
                ulong lo = 0, hi = 0;
                for (int s = 0; s < b.K + b.M; s++)
                    if (!b.Present[s]) { if (s < 64) lo |= 1UL << s; else hi |= 1UL << (s - 64); }
                b.Nacks++;
                output.Add(new UdpmWire.Nack(f.Id, (byte)bi, streamType, lo, hi));
            }
    }
}

// One socket for the console's video and audio (UDPM types 1 and 2): Hello keepalive, NAT-assisted endpoint discovery,
// reassembly + FEC, NACKs, keyframe requests and feedback. Complete frames are the console's original wire packets
// (18-byte NxRemotePlay header + payload); they go to onVideoWire / onAudioWire on the channel's own thread.
internal sealed class UdpmClientChannel : IDisposable
{
    static readonly byte[] NatTag = { (byte)'N', (byte)'U', (byte)'P', (byte)'1' };
    internal const uint NatMagic = 0x3150554e;

    readonly IPAddress console;
    readonly int port;
    readonly uint session;
    readonly byte[]? natKey;
    readonly IPEndPoint? rendezvous;
    readonly Action<byte[]> onVideoWire, onAudioWire;
    readonly Action<string> onEnd, onLog;
    readonly long silentLimitUs;
    Thread? worker;
    volatile bool running;
    volatile int keyframesWanted;

    public UdpmClientChannel(IPAddress console, int port, uint session, byte[]? natKey, IPEndPoint? rendezvous,
        Action<byte[]> onVideoWire, Action<byte[]> onAudioWire, Action<string> onEnd, Action<string> onLog, long silentLimitMs = 15_000)
    {
        this.console = console; this.port = port; this.session = session;
        this.natKey = natKey is { Length: 16 } ? natKey : null; this.rendezvous = rendezvous;
        this.onVideoWire = onVideoWire; this.onAudioWire = onAudioWire; this.onEnd = onEnd; this.onLog = onLog;
        silentLimitUs = silentLimitMs * 1000;
    }

    public UdpmClientChannel Start()
    {
        running = true;
        worker = new Thread(Run) { IsBackground = true, Name = "udpm-client" };
        worker.Start();
        return this;
    }

    public void Dispose()
    {
        running = false;
        worker?.Join(500);
        worker = null;
    }

    static long NowUs() => MonotonicClock.MicrosecondsNow;

    internal static byte[] NatPacket(byte role, uint session, byte[] key)
    {
        var p = new byte[28];
        NatTag.CopyTo(p, 0);
        p[4] = role;
        BinaryPrimitives.WriteUInt32LittleEndian(p.AsSpan(8), session);
        key.CopyTo(p, 12);
        return p;
    }

    bool NatMatches(ReadOnlySpan<byte> d)
    {
        if (natKey is null || (d.Length != 28 && d.Length != 36)) return false;
        return d[..28].SequenceEqual(NatPacket(d[4], session, natKey));
    }

    // The peer the rendezvous server saw for the console, from a role-3 reply.
    IPEndPoint? NatPeer(ReadOnlySpan<byte> d)
    {
        if (d.Length != 36 || !NatMatches(d) || d[4] != 3 || d[34] != 0 || d[35] != 0) return null;
        var address = new IPAddress(d.Slice(28, 4));
        int peerPort = (d[32] << 8) | d[33];
        if (peerPort == 0 || address.Equals(IPAddress.Any) || d[28] >= 224) return null;
        return new IPEndPoint(address, peerPort);
    }

    void Run()
    {
        Socket? sock = null;
        UdpmStreamReceiver? rx = null, rxAudio = null;
        long nacks = 0, audioNacks = 0, keyReqs = 0;
        try
        {
            sock = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
            sock.ReceiveBufferSize = 4 << 20;
            // A "port unreachable" answer to an earlier send must never end the receive loop.
            if (OperatingSystem.IsWindows()) try { sock.IOControl(unchecked((int)0x9800000C), new byte[] { 0 }, null); } catch { }
            sock.Bind(new IPEndPoint(IPAddress.Any, 0));
            var target = new IPEndPoint(console, port);
            IPEndPoint? alt = null;
            // Complete video frames wait in `held` until their presentation time, so bursty arrival leaves evenly spaced
            // (the player shows one picture per refresh in order). Audio goes straight on, as on Android.
            var schedule = new PresentationSchedule();
            var congestion = new CongestionMeter();
            var held = new Queue<(long Due, byte[] Wire)>();
            long frameClock = 0;
            rx = new UdpmStreamReceiver(UdpmWire.TypeVideo, session, (_, data, timestampUs, _) =>
            {
                long due = frameClock;
                if (timestampUs != 0)
                {
                    congestion.OnFrame((long)timestampUs, frameClock);
                    due = schedule.PresentAt((long)timestampUs, frameClock);
                }
                if (due <= frameClock && held.Count == 0) { onVideoWire(data); return; }
                held.Enqueue((due, data));
                // The schedule waits at most 80 ms (about five frames); never let the queue grow beyond that.
                while (held.Count > 16) onVideoWire(held.Dequeue().Wire);
            });
            rxAudio = new UdpmStreamReceiver(UdpmWire.TypeAudio, session, (_, data, _, _) => onAudioWire(data));
            var buffer = new byte[UdpmWire.MaxDatagram + 64];
            EndPoint from = new IPEndPoint(IPAddress.Any, 0);
            var nackList = new List<UdpmWire.Nack>();
            long lastHeard = NowUs(), firstPacket = 0, nextHello = 0, nextFb = 0, nextKey = 0, nextLog = lastHeard + 5_000_000, nextRegister = 0;
            uint fbSeq = 0;
            int regSent = 0, relayRx = 0, otherRx = 0;

            void Send(byte[] data, IPEndPoint to) { try { sock.SendTo(data, to); } catch (SocketException) { } }
            onLog($"UDPM media: listening for console {console}:{port} session {session:x}");
            while (running)
            {
                long t0 = NowUs();
                if (firstPacket == 0 && natKey is not null && rendezvous is not null && t0 >= nextRegister)
                {
                    try { sock.SendTo(NatPacket(2, session, natKey), rendezvous); regSent++; } catch (SocketException) { }
                    if (regSent == 1) onLog($"rendezvous: first registration sent to {rendezvous} from local port {((IPEndPoint)sock.LocalEndPoint!).Port}");
                    // Keep opening the peer pinhole from this very same media socket.
                    Send(NatPacket(4, session, natKey), target);
                    if (alt is not null) Send(NatPacket(4, session, natKey), alt);
                    nextRegister = t0 + 250_000;
                }
                // Hello twice a second until media flows, then once a second (NAT keepalive + liveness).
                if (t0 >= nextHello)
                {
                    var hello = UdpmWire.WriteHello(session, fbSeq);
                    Send(hello, target);
                    if (alt is not null) Send(hello, alt);
                    nextHello = t0 + (firstPacket == 0 ? 500_000 : 1_000_000);
                }
                // Bounded so a steady stream cannot starve the timers below.
                for (int burst = 0; burst < 128 && sock.Poll(burst == 0 ? 2000 : 0, SelectMode.SelectRead); burst++)
                {
                    int n;
                    try { n = sock.ReceiveFrom(buffer, ref from); }
                    catch (SocketException) { continue; }
                    var source = (IPEndPoint)from;
                    var datagram = new ReadOnlySpan<byte>(buffer, 0, n);
                    if (natKey is not null && rendezvous is not null && source.Equals(rendezvous))
                    {
                        relayRx++;
                        var peer = NatPeer(datagram);
                        if (peer is not null)
                        {
                            if (!peer.Equals(target) && !peer.Equals(alt) && firstPacket == 0)
                            {
                                alt = peer;
                                onLog($"UDP rendezvous: extra endpoint {peer} (keeping console address {target})");
                            }
                            if (alt is not null) { Send(NatPacket(4, session, natKey), alt); Send(UdpmWire.WriteHello(session, fbSeq), alt); }
                        }
                        continue;
                    }
                    bool fromTarget = source.Equals(target), fromAlt = alt is not null && source.Equals(alt);
                    if (!fromTarget && !fromAlt) { otherRx++; continue; }
                    if (fromAlt && !fromTarget && firstPacket == 0)
                    {
                        onLog($"UDP: first packet arrived from {alt}");
                        target = alt!; alt = null;
                    }
                    else if (fromTarget && firstPacket == 0) alt = null;
                    if (natKey is not null && NatMatches(datagram) && datagram[4] == 4)
                    {
                        Send(UdpmWire.WriteHello(session, fbSeq), target);
                        continue;
                    }
                    long now = NowUs();
                    frameClock = now;
                    if (rx.Push(datagram, now) || rxAudio.Push(datagram, now))
                    {
                        lastHeard = now;
                        if (firstPacket == 0) { firstPacket = lastHeard; onLog("UDPM media: first datagram received"); }
                    }
                }
                long t = NowUs();
                frameClock = t;
                rx.Poll(t);
                while (held.Count > 0 && held.Peek().Due <= t) onVideoWire(held.Dequeue().Wire);
                nackList.Clear();
                rx.CollectNacks(t, nackList);
                foreach (var nack in nackList) { Send(UdpmWire.WriteNack(session, nack), target); nacks++; }
                rxAudio.Poll(t);
                nackList.Clear();
                rxAudio.CollectNacks(t, nackList);
                foreach (var nack in nackList) { Send(UdpmWire.WriteNack(session, nack), target); audioNacks++; }
                if (rx.WaitingKeyframe && firstPacket != 0 && t - firstPacket > 300_000 && t >= nextKey)
                {
                    Send(UdpmWire.WriteKeyframeRequest(session, rx.NextFrameId - 1), target);
                    keyReqs++; nextKey = t + 250_000;
                }
                if (keyframesWanted > 0 && t >= nextKey)
                {
                    Send(UdpmWire.WriteKeyframeRequest(session, rx.NextFrameId - 1), target);
                    keyReqs++; keyframesWanted--; nextKey = t + 100_000;
                }
                if (t >= nextFb)
                {
                    Send(UdpmWire.WriteFeedback(session, new UdpmWire.Feedback(fbSeq++, rx.NextFrameId, (uint)rx.PacketsReceived, (uint)rx.PacketsLost,
                        (uint)rx.ShardsRecoveredFec, (uint)rx.FramesDropped, (uint)rx.JitterUs, (uint)schedule.TargetDelayUs)), target);
                    nextFb = t + 250_000;
                }
                // The desktop player keeps no audio-underrun count to share; the host also reacts to the delay fields alone.
                if (firstPacket != 0 && congestion.Report(t, (uint)nacks, 0) is { } report)
                    Send(UdpmWire.WriteCongestion(session, report), target);
                if (t >= nextLog)
                {
                    if (natKey is not null) onLog($"rendezvous 5s: registrations sent={regSent} relay_replies={relayRx} other_source_rx={otherRx}");
                    onLog($"UDPM video 5s: delivered={rx.FramesDelivered} pkts={rx.PacketsReceived} lost={rx.PacketsLost} fec={rx.ShardsRecoveredFec} nacks={nacks} keyreq={keyReqs} dropped={rx.FramesDropped} skipped={rx.FramesSkippedWaitingKeyframe} jitter={rx.JitterUs / 1000.0:0.0}ms playout={schedule.TargetDelayUs / 1000.0:0.0}ms {congestion.Summary()}");
                    onLog($"UDPM audio 5s: delivered={rxAudio.FramesDelivered} pkts={rxAudio.PacketsReceived} lost={rxAudio.PacketsLost} fec={rxAudio.ShardsRecoveredFec} nacks={audioNacks} dropped={rxAudio.FramesDropped}");
                    nextLog = t + 5_000_000;
                }
                if (t - lastHeard > silentLimitUs)
                {
                    onEnd($"UDP media silent for {silentLimitUs / 1_000_000} s");
                    break;
                }
            }
        }
        catch (Exception ex)
        {
            if (running) onEnd($"UDP media failed: {ex.GetType().Name}: {ex.Message}");
        }
        finally
        {
            sock?.Dispose();
            if (rx is not null)
                onLog($"UDPM video total: delivered={rx.FramesDelivered} pkts={rx.PacketsReceived} lost={rx.PacketsLost} fec={rx.ShardsRecoveredFec} dropped={rx.FramesDropped} skipped={rx.FramesSkippedWaitingKeyframe}");
        }
    }
}
