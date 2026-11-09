using NxRemotePlay.Client.Core;
using System;
using System.Diagnostics;
using System.Threading;
using System.Threading.Tasks;

namespace NxRemotePlay.Client.Bridge;

// UDPM sender: splits one access unit into FEC blocks, paces the datagrams with a token bucket and keeps recent
// datagrams for NACK retransmission. Same shard layout, FEC and pacing arithmetic as Host/source/udpm/udpm_sender.c.
// One caller sends frames; NACK retransmits may arrive concurrently from the receive loop.
internal sealed class UdpmSender
{
    internal sealed class Config
    {
        internal int ShardSize = UdpmWire.DefaultShard, MaxDataShards = 32, FecPercent = 20, MinParity = 2, MaxParity = 12;
        internal uint PaceBps = 20_000_000, BurstBytes = 16 * 1024;
    }
    internal struct Stats
    {
        internal uint Frames, Datagrams, ParityDatagrams, Retransmits, SendErrors;
        internal ulong PacedWaitUs, MaxSendUs;
    }
    sealed class Slot
    {
        internal byte Type, Block, Shard; internal uint FrameId; internal int Length; internal bool Valid;
        internal readonly byte[] Data = new byte[UdpmWire.MaxDatagram];
    }
    readonly Config cfg;
    readonly uint session;
    readonly Func<ReadOnlyMemory<byte>, bool> send;
    readonly uint[] nextFrame = new uint[4], packetSeq = new uint[4];
    readonly Slot[] rtx;
    readonly object gate = new();
    readonly byte[] blockData, parity, datagram = new byte[UdpmWire.MaxDatagram], nackBuffer = new byte[UdpmWire.MaxDatagram];
    int rtxNext;
    ulong tokensX1e6, lastUs; bool primed; uint rateBps;
    Stats stats;
    internal UdpmSender(Config cfg, uint session, int rtxSlots, Func<ReadOnlyMemory<byte>, bool> send)
    {
        this.cfg = cfg; this.session = session; this.send = send;
        if (cfg.ShardSize is < 1 or > UdpmWire.MaxShard) cfg.ShardSize = UdpmWire.DefaultShard;
        if (cfg.MaxDataShards is < 1 or > UdpmWire.MaxDataShards) cfg.MaxDataShards = 32;
        if (cfg.MaxParity > UdpmWire.MaxParityShards) cfg.MaxParity = UdpmWire.MaxParityShards;
        if (cfg.MinParity > cfg.MaxParity) cfg.MinParity = cfg.MaxParity;
        rtx = new Slot[rtxSlots]; for (int i = 0; i < rtx.Length; i++) rtx[i] = new Slot();
        blockData = new byte[cfg.ShardSize * cfg.MaxDataShards]; parity = new byte[cfg.ShardSize * cfg.MaxParity];
        rateBps = cfg.PaceBps == 0 ? 1 : cfg.PaceBps;
    }
    internal Stats TakeStats() { lock (gate) { var s = stats; stats = default; return s; } }
    internal void SetPace(uint bps) { lock (gate) rateBps = bps == 0 ? 1 : bps; }
    internal int ParityFor(int k)
    {
        int m = (k * cfg.FecPercent + 99) / 100;
        if (m < cfg.MinParity) m = cfg.MinParity;
        if (m > cfg.MaxParity) m = cfg.MaxParity;
        if (k + m > 255) m = 255 - k;
        return m;
    }
    static ulong NowUs() => (ulong)MonotonicClock.MicrosecondsNow;

    // 0 when `bytes` may leave now (tokens consumed), otherwise microseconds to wait.
    ulong PacerDelayUs(ulong nowUs, uint bytes)
    {
        lock (gate)
        {
            ulong cap = (ulong)cfg.BurstBytes * 1_000_000UL;
            if (!primed) { tokensX1e6 = cap; lastUs = nowUs; primed = true; }
            if (nowUs > lastUs)
            {
                ulong add = (nowUs - lastUs) * (rateBps / 8);
                tokensX1e6 = tokensX1e6 + add > cap ? cap : tokensX1e6 + add; lastUs = nowUs;
            }
            ulong need = (ulong)bytes * 1_000_000UL;
            if (tokensX1e6 >= need) { tokensX1e6 -= need; return 0; }
            ulong perSec = rateBps / 8 != 0 ? rateBps / 8 : 1;
            return (need - tokensX1e6 + perSec - 1) / perSec;
        }
    }
    static async ValueTask WaitUs(ulong us, CancellationToken cancel)
    {
        long end = Stopwatch.GetTimestamp() + (long)us * Stopwatch.Frequency / 1_000_000;
        while (true)
        {
            long remaining = end - Stopwatch.GetTimestamp();
            if (remaining <= 0) return;
            if (remaining * 1_000_000 / Stopwatch.Frequency > 2500) await Task.Delay(1, cancel).ConfigureAwait(false);
            else Thread.SpinWait(40);
        }
    }
    async ValueTask<bool> Emit(byte[] dg, int length, CancellationToken cancel)
    {
        ulong delay = PacerDelayUs(NowUs(), (uint)length);
        if (delay != 0)
        {
            await WaitUs(delay, cancel).ConfigureAwait(false);
            lock (gate) stats.PacedWaitUs += delay;
            PacerDelayUs(NowUs(), (uint)length); // consume after waiting
        }
        ulong t0 = NowUs();
        bool ok = send(dg.AsMemory(0, length));
        ulong took = NowUs() - t0;
        lock (gate)
        {
            if (took > stats.MaxSendUs) stats.MaxSendUs = took;
            if (!ok) { stats.SendErrors++; return false; }
            stats.Datagrams++;
        }
        return true;
    }
    void Cache(byte type, uint frameId, byte block, byte shard, byte[] dg, int length)
    {
        lock (gate)
        {
            var slot = rtx[rtxNext]; rtxNext = (rtxNext + 1) % rtx.Length;
            slot.Type = type; slot.FrameId = frameId; slot.Block = block; slot.Shard = shard; slot.Length = length; slot.Valid = true;
            Buffer.BlockCopy(dg, 0, slot.Data, 0, length);
        }
    }
    // Sends one access unit (video wire packet = 18-byte NxRemotePlay header + payload). Returns the frame id or -1 on a send error.
    internal async ValueTask<long> SendFrame(byte type, ReadOnlyMemory<byte> frame, ulong timestampUs, byte flags, CancellationToken cancel)
    {
        if (type is < UdpmWire.TypeVideo or > UdpmWire.TypeProbe || frame.Length == 0) return -1;
        int S = cfg.ShardSize, K = cfg.MaxDataShards, size = frame.Length;
        long blockBytes = (long)S * K;
        long blocks = (size + blockBytes - 1) / blockBytes;
        if (blocks > 255) return -1;
        uint frameId; lock (gate) frameId = nextFrame[type]++;
        for (int b = 0; b < blocks; b++)
        {
            long off = b * blockBytes;
            int len = (int)Math.Min(blockBytes, size - off);
            int k = (len + S - 1) / S, m = ParityFor(k);
            EncodeBlock(frame, (int)off, len, k, m);
            for (int i = 0; i < k + m; i++)
            {
                bool isParity = i >= k; uint seq; lock (gate) seq = packetSeq[type]++;
                int dgLength = BuildDatagram(type, flags, seq, frameId, size, b, (int)blocks, i, k, m, timestampUs);
                Cache(type, frameId, (byte)b, (byte)i, datagram, dgLength);
                if (!await Emit(datagram, dgLength, cancel).ConfigureAwait(false)) return -1;
                if (isParity) lock (gate) stats.ParityDatagrams++;
            }
        }
        lock (gate) stats.Frames++;
        return frameId;
    }
    // Span work stays out of the async method: it cannot be held across an await.
    void EncodeBlock(ReadOnlyMemory<byte> frame, int offset, int length, int k, int m)
    {
        int S = cfg.ShardSize;
        var data = blockData.AsSpan(0, k * S);
        frame.Span.Slice(offset, length).CopyTo(data);
        data[length..].Clear(); // zero-pad the last shard
        if (m != 0) UdpmFec.Encode(data, k, parity.AsSpan(0, m * S), m, S);
    }
    int BuildDatagram(byte type, byte flags, uint seq, uint frameId, int frameSize, int block, int blocks, int index, int k, int m, ulong timestampUs)
    {
        int S = cfg.ShardSize; bool isParity = index >= k;
        UdpmWire.WriteShardHeader(datagram, type, (byte)((flags & UdpmWire.FlagKeyframe) | (isParity ? UdpmWire.FlagParity : 0)), session, seq, frameId,
            (uint)frameSize, (byte)block, (byte)blocks, (byte)index, (byte)k, (byte)m, (ushort)S, timestampUs);
        (isParity ? parity.AsSpan((index - k) * S, S) : blockData.AsSpan(index * S, S)).CopyTo(datagram.AsSpan(UdpmWire.HeaderSize));
        return UdpmWire.HeaderSize + S;
    }
    // Resends the shards named by a NACK that are still cached (paced tokens are consumed but never waited for).
    internal int HandleNack(UdpmWire.Nack n)
    {
        int resent = 0;
        lock (gate)
        {
            foreach (var slot in rtx)
            {
                if (!slot.Valid || slot.Type != n.StreamType || slot.FrameId != n.FrameId || slot.Block != n.BlockIndex) continue;
                int idx = slot.Shard;
                ulong bit = idx < 64 ? (n.MissingLo >> idx) & 1 : (n.MissingHi >> (idx - 64)) & 1;
                if (bit == 0) continue;
                Buffer.BlockCopy(slot.Data, 0, nackBuffer, 0, slot.Length);
                nackBuffer[3] |= UdpmWire.FlagRetransmit;
                uint seq = packetSeq[slot.Type]++;
                System.Buffers.Binary.BinaryPrimitives.WriteUInt32LittleEndian(nackBuffer.AsSpan(8), seq);
                if (!send(nackBuffer.AsMemory(0, slot.Length))) { stats.SendErrors++; break; }
                stats.Retransmits++; stats.Datagrams++; resent++;
            }
        }
        return resent;
    }
}
