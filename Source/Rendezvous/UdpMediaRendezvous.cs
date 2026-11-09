using System.Buffers.Binary;
using System.Net;

namespace Rendezvous;

// endpoint discovery only. No media forwarding, identities or owner keys.
// A random 128-bit capability is passed to the peer inside the authenticated TCP
// stream handshake. It is never logged. Entries expire after 10 seconds.
internal sealed class UdpMediaRendezvous
{
    internal const uint Magic = 0x3150554e; // NUP1
    sealed class Pair {
        public IPEndPoint? Host, Client;
        public long Seen;
    }
    readonly Dictionary<string, Pair> pairs = new();
    internal Action<string>? Log;
    readonly HashSet<string> logged = new();
    internal List<(IPEndPoint To, byte[] Data)> Handle(ReadOnlySpan<byte> data, IPEndPoint from, long nowMs)
    {
        var result = new List<(IPEndPoint, byte[])>();
        if (data.Length != 28 || BinaryPrimitives.ReadUInt32LittleEndian(data) != Magic ||
            data[4] is not (1 or 2) || data[5] != 0 || data[6] != 0 || data[7] != 0 ||
            from.Address.AddressFamily != System.Net.Sockets.AddressFamily.InterNetwork) return result;
        // Run from the server's single UDP reader. Bound attacker-controlled storage.
        foreach (var key in pairs.Where(p => nowMs - p.Value.Seen > 10000).Select(p => p.Key).ToArray()) pairs.Remove(key);
        var id = Convert.ToHexString(data[8..28]); // session + capability
        if (!pairs.TryGetValue(id, out var pair)) {
            if (pairs.Count >= 1024) return result;
            pair = new Pair(); pairs.Add(id, pair);
        }
        pair.Seen = nowMs;
        var sess = Convert.ToHexString(data[8..12]); // session only; the capability is never logged
        if (logged.Count > 256) logged.Clear();
        if (logged.Add(sess + data[4])) Log?.Invoke($"NUP1 {(data[4] == 1 ? "host" : "client")} registration from {from} session {sess}");
        if (data[4] == 1) pair.Host = from; else pair.Client = from;
        if (pair.Host is null || pair.Client is null) return result;
        if (logged.Add(sess + "match")) Log?.Invoke($"NUP1 session {sess} matched: host {pair.Host} client {pair.Client} - replies sent");
        var dataCopy = data.ToArray();
        byte[] Reply(IPEndPoint peer) {
            var b = new byte[36]; dataCopy.CopyTo(b, 0); b[4] = 3;
            peer.Address.GetAddressBytes().CopyTo(b, 28);
            BinaryPrimitives.WriteUInt16BigEndian(b.AsSpan(32), (ushort)peer.Port);
            return b;
        }
        result.Add((pair.Host, Reply(pair.Client)));
        result.Add((pair.Client, Reply(pair.Host)));
        return result;
    }
}
