using NxRemotePlay.Client.Core;
using System;
using System.Runtime.InteropServices;
using System.Threading.Tasks;

namespace NxRemotePlay.Client.Sources;

// USB bulk transfers do not guarantee one protocol packet per read. Keep trailing bytes
// and assemble bounded packets even when transfers split a header or combine media/replies.
internal sealed class UsbPacketReader
{
    readonly Func<byte[], Task<int>> read;
    readonly byte[] buffer = new byte[PacketHeader.MaxTransferSize];
    readonly byte[] headerBytes = new byte[PacketHeader.StructLength];
    int offset, count;
    internal UsbPacketReader(Func<byte[], Task<int>> read) => this.read = read;

    // One media read. A read that times out with nothing received only means the console has nothing to send yet (for
    // example the game-capture to HOME-still switch takes a few seconds): the link is still up, so keep waiting. A timeout
    // that did deliver bytes keeps them. A real disconnect or transfer error comes back at once with another error code and
    // still ends the session immediately. The console ends its own USB session after 5 s without sending, so `idleLimit`
    // only matters when the console hangs while the cable stays connected.
    internal static async Task<int> ReadPatiently(Func<Task<(LibUsbDotNet.Error error, int transferLength)>> readOnce, TimeSpan idleLimit, System.Threading.CancellationToken cancel)
    {
        long waitStart = System.Diagnostics.Stopwatch.GetTimestamp();
        while (true)
        {
            var (err, count) = await readOnce().ConfigureAwait(false);
            if (err == LibUsbDotNet.Error.Success || (err == LibUsbDotNet.Error.Timeout && count > 0)) return count;
            if (err != LibUsbDotNet.Error.Timeout) throw new Exception("USB media read failed: " + err);
            cancel.ThrowIfCancellationRequested();
            if (System.Diagnostics.Stopwatch.GetElapsedTime(waitStart) >= idleLimit)
                throw new Exception("USB media read failed: no data for " + (int)idleLimit.TotalSeconds + " s");
        }
    }
    internal void Reset() { offset = count = 0; }
    async Task Exact(Memory<byte> destination)
    {
        int have = 0;
        while (have < destination.Length)
        {
            if (offset == count)
            {
                count = await read(buffer).ConfigureAwait(false); offset = 0;
                if (count <= 0 || count > buffer.Length) throw new Exception("Invalid USB transfer length.");
            }
            int copy = Math.Min(count - offset, destination.Length - have);
            buffer.AsMemory(offset, copy).CopyTo(destination.Slice(have, copy));
            have += copy; offset += copy;
        }
    }
    internal async Task<ReceivedPacket> Next()
    {
        await Exact(headerBytes).ConfigureAwait(false);
        var header = MemoryMarshal.Read<PacketHeader>(headerBytes);
        bool owner = header.Magic == UsbOwnerProtocol.ResponseMagic || header.Magic == UsbDelegationProtocol.ResponseMagic || header.Magic == UsbEnrollmentProtocol.ResponseMagic;
        if ((header.Magic != PacketHeader.MagicResponse && !owner) ||
            header.DataSize < 0 || header.DataSize > StreamInfo.MaxPayloadSize ||
            (owner && header.DataSize != 64))
            throw new Exception("Invalid USB packet header or payload length.");
        PoolBuffer? data = null;
        try
        {
            if (header.DataSize != 0) { data = PoolBuffer.Rent(header.DataSize); await Exact(data.Memory).ConfigureAwait(false); }
            return new ReceivedPacket(header, data);
        }
        catch { data?.Free(); throw; }
    }
}
