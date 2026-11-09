using System;
using System.Buffers.Binary;

namespace NxRemotePlay.Client.Bridge;

// UDPM wire format and Reed-Solomon FEC. Byte-identical to Host/source/udpm (udpm_proto.c, udpm_fec.c) and to the
// Android receiver (udpm/UdpmProto.kt, UdpmFec.kt); every multi-byte field is little-endian.
internal static class UdpmWire
{
    internal const byte Magic = 0xD7, Version = 1;
    internal const byte TypeVideo = 1, TypeAudio = 2, TypeProbe = 3, TypeHello = 0x10, TypeNack = 0x11, TypeFeedback = 0x12, TypeKeyframeRequest = 0x13, TypeCongestion = 0x14;
    internal const byte FlagKeyframe = 1, FlagParity = 2, FlagRetransmit = 4;
    internal const int HeaderSize = 36, DefaultShard = 1200, MaxShard = 1400, MaxDatagram = HeaderSize + MaxShard, MaxDataShards = 64, MaxParityShards = 32;
    internal const int NackSize = 32, FeedbackSize = 40, CongestionSize = 40, KeyframeRequestSize = 12, HelloSize = 12;

    internal readonly record struct Nack(uint FrameId, byte BlockIndex, byte StreamType, ulong MissingLo, ulong MissingHi);
    internal readonly record struct Feedback(uint Seq, uint HighestFrameId, uint PacketsReceived, uint PacketsLost,
        uint ShardsRecoveredFec, uint FramesDropped, uint JitterUs, uint PlayoutDelayUs);
    // Client -> host, every ~250 ms: transit above the best case seen (smallest and largest), NACKs and audio underruns
    // in the interval; the last body word is reserved (0).
    internal readonly record struct Congestion(uint Seq, uint IntervalMs, uint Frames, uint ExcessMinUs, uint ExcessMaxUs, uint Nacks, uint AudioUnderruns);

    internal static int PeekType(ReadOnlySpan<byte> d) => d.Length < 8 || d[0] != Magic || d[1] != Version ? 0 : d[2];

    static void Prefix(Span<byte> o, byte type, byte flags, uint session)
    { o[0] = Magic; o[1] = Version; o[2] = type; o[3] = flags; BinaryPrimitives.WriteUInt32LittleEndian(o[4..], session); }
    static bool CheckPrefix(ReadOnlySpan<byte> d, int want, byte type, uint session) =>
        d.Length == want && d[0] == Magic && d[1] == Version && d[2] == type && BinaryPrimitives.ReadUInt32LittleEndian(d[4..]) == session;

    internal static void WriteShardHeader(Span<byte> o, byte type, byte flags, uint session, uint packetSeq, uint frameId, uint frameSize,
        byte blockIndex, byte blockCount, byte shardIndex, byte dataShards, byte parityShards, ushort shardSize, ulong timestampUs)
    {
        Prefix(o, type, flags, session);
        BinaryPrimitives.WriteUInt32LittleEndian(o[8..], packetSeq);
        BinaryPrimitives.WriteUInt32LittleEndian(o[12..], frameId);
        BinaryPrimitives.WriteUInt32LittleEndian(o[16..], frameSize);
        o[20] = blockIndex; o[21] = blockCount; o[22] = shardIndex; o[23] = dataShards; o[24] = parityShards; o[25] = 0;
        BinaryPrimitives.WriteUInt16LittleEndian(o[26..], shardSize);
        BinaryPrimitives.WriteUInt64LittleEndian(o[28..], timestampUs);
    }

    internal static bool TryReadHello(ReadOnlySpan<byte> d, uint session, out uint nonce)
    {
        nonce = 0; if (!CheckPrefix(d, HelloSize, TypeHello, session)) return false;
        nonce = BinaryPrimitives.ReadUInt32LittleEndian(d[8..]); return true;
    }
    internal static bool TryReadNack(ReadOnlySpan<byte> d, uint session, out Nack n)
    {
        n = default; if (!CheckPrefix(d, NackSize, TypeNack, session)) return false;
        ulong lo = BinaryPrimitives.ReadUInt64LittleEndian(d[16..]), hi = BinaryPrimitives.ReadUInt64LittleEndian(d[24..]);
        n = new Nack(BinaryPrimitives.ReadUInt32LittleEndian(d[8..]), d[12], d[13], lo, hi);
        return lo != 0 || hi != 0;
    }
    internal static bool TryReadFeedback(ReadOnlySpan<byte> d, uint session, out Feedback f)
    {
        f = default; if (!CheckPrefix(d, FeedbackSize, TypeFeedback, session)) return false;
        f = new Feedback(BinaryPrimitives.ReadUInt32LittleEndian(d[8..]), BinaryPrimitives.ReadUInt32LittleEndian(d[12..]), BinaryPrimitives.ReadUInt32LittleEndian(d[16..]),
            BinaryPrimitives.ReadUInt32LittleEndian(d[20..]), BinaryPrimitives.ReadUInt32LittleEndian(d[24..]), BinaryPrimitives.ReadUInt32LittleEndian(d[28..]),
            BinaryPrimitives.ReadUInt32LittleEndian(d[32..]), BinaryPrimitives.ReadUInt32LittleEndian(d[36..])); return true;
    }
    internal static bool TryReadKeyframeRequest(ReadOnlySpan<byte> d, uint session, out uint lastGoodFrameId)
    {
        lastGoodFrameId = 0; if (!CheckPrefix(d, KeyframeRequestSize, TypeKeyframeRequest, session)) return false;
        lastGoodFrameId = BinaryPrimitives.ReadUInt32LittleEndian(d[8..]); return true;
    }
    // Writers for the datagrams a receiving client sends back (the desktop as a client, and the tests).
    internal static byte[] WriteHello(uint session, uint nonce)
    { var b = new byte[HelloSize]; Prefix(b, TypeHello, 0, session); BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(8), nonce); return b; }

    internal static byte[] WriteKeyframeRequest(uint session, uint lastGoodFrameId)
    { var b = new byte[KeyframeRequestSize]; Prefix(b, TypeKeyframeRequest, 0, session); BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(8), lastGoodFrameId); return b; }

    internal static byte[] WriteNack(uint session, Nack n)
    {
        var b = new byte[NackSize]; Prefix(b, TypeNack, 0, session);
        BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(8), n.FrameId); b[12] = n.BlockIndex; b[13] = n.StreamType;
        BinaryPrimitives.WriteUInt64LittleEndian(b.AsSpan(16), n.MissingLo); BinaryPrimitives.WriteUInt64LittleEndian(b.AsSpan(24), n.MissingHi); return b;
    }

    internal static byte[] WriteFeedback(uint session, Feedback f)
    {
        var b = new byte[FeedbackSize]; Prefix(b, TypeFeedback, 0, session);
        uint[] v = { f.Seq, f.HighestFrameId, f.PacketsReceived, f.PacketsLost, f.ShardsRecoveredFec, f.FramesDropped, f.JitterUs, f.PlayoutDelayUs };
        for (int i = 0; i < 8; i++) BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(8 + 4 * i), v[i]); return b;
    }

    internal static byte[] WriteCongestion(uint session, Congestion c)
    {
        var b = new byte[CongestionSize]; Prefix(b, TypeCongestion, 0, session);
        uint[] v = { c.Seq, c.IntervalMs, c.Frames, c.ExcessMinUs, c.ExcessMaxUs, c.Nacks, c.AudioUnderruns, 0 };
        for (int i = 0; i < 8; i++) BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(8 + 4 * i), v[i]); return b;
    }

}

// Systematic Reed-Solomon erasure code over GF(2^8), polynomial 0x11D; parity rows form a Cauchy matrix.
internal static class UdpmFec
{
    static readonly byte[] exp = new byte[512], log = new byte[256];
    static UdpmFec()
    {
        int x = 1;
        for (int i = 0; i < 255; i++) { exp[i] = (byte)x; log[x] = (byte)i; x <<= 1; if ((x & 0x100) != 0) x ^= 0x11D; }
        for (int i = 255; i < 512; i++) exp[i] = exp[i - 255];
    }
    internal static byte Inv(byte a) => exp[255 - log[a]];
    static byte Mul(byte a, byte b) => a == 0 || b == 0 ? (byte)0 : exp[log[a] + log[b]];
    // Cauchy element 1/(x_p ^ y_d) with x_p = p and y_d = m + d.
    internal static byte Coeff(int m, int p, int d) => Inv((byte)(p ^ (m + d)));
    static void MulAdd(Span<byte> dst, ReadOnlySpan<byte> src, byte c)
    {
        if (c == 0) return;
        if (c == 1) { for (int i = 0; i < dst.Length; i++) dst[i] ^= src[i]; return; }
        int lc = log[c];
        for (int i = 0; i < dst.Length; i++) { byte s = src[i]; if (s != 0) dst[i] ^= exp[log[s] + lc]; }
    }
    // data: k shards of `size` bytes back to back; parity: m shards of `size` bytes back to back (overwritten).
    internal static void Encode(ReadOnlySpan<byte> data, int k, Span<byte> parity, int m, int size)
    {
        for (int p = 0; p < m; p++)
        {
            var dst = parity.Slice(p * size, size); dst.Clear();
            for (int d = 0; d < k; d++) MulAdd(dst, data.Slice(d * size, size), Coeff(m, p, d));
        }
    }

    // Mirror of udpm_fec.c / UdpmFec.kt decode. buf holds k data then m parity shards of `size` bytes; present marks
    // which arrived. Rebuilds the missing data shards in place and returns how many, or -1 with fewer than k shards.
    internal static int Decode(byte[] buf, bool[] present, int k, int m, int size)
    {
        var missing = new System.Collections.Generic.List<int>();
        for (int i = 0; i < k; i++) if (!present[i]) missing.Add(i);
        if (missing.Count == 0) return 0;
        var rows = new System.Collections.Generic.List<int>();
        for (int i = 0; i < k + m && rows.Count < k; i++) if (present[i]) rows.Add(i);
        if (rows.Count < k) return -1;
        var a = new byte[k][]; var inv = new byte[k][];
        for (int r = 0; r < k; r++)
        {
            a[r] = new byte[k]; inv[r] = new byte[k]; inv[r][r] = 1;
            int s = rows[r];
            for (int c = 0; c < k; c++) a[r][c] = s < k ? (byte)(s == c ? 1 : 0) : Coeff(m, s - k, c);
        }
        for (int c = 0; c < k; c++)
        {
            int piv = c;
            while (piv < k && a[piv][c] == 0) piv++;
            if (piv == k) return -1;
            if (piv != c) { (a[c], a[piv]) = (a[piv], a[c]); (inv[c], inv[piv]) = (inv[piv], inv[c]); }
            byte f = Inv(a[c][c]);
            for (int j = 0; j < k; j++) { a[c][j] = Mul(a[c][j], f); inv[c][j] = Mul(inv[c][j], f); }
            for (int r = 0; r < k; r++)
            {
                if (r == c || a[r][c] == 0) continue;
                byte g = a[r][c];
                for (int j = 0; j < k; j++) { a[r][j] ^= Mul(g, a[c][j]); inv[r][j] ^= Mul(g, inv[c][j]); }
            }
        }
        foreach (int d in missing)
        {
            var dst = buf.AsSpan(d * size, size); dst.Clear();
            for (int r = 0; r < k; r++) MulAdd(dst, buf.AsSpan(rows[r] * size, size), inv[d][r]);
        }
        return missing.Count;
    }
}
