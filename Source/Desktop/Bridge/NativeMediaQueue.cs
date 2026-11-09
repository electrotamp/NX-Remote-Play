using NxRemotePlay.Client.Core;
using NxRemotePlay.Client.Sources;
using System;
using System.Threading;
using System.Threading.Channels;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Diagnostics;

namespace NxRemotePlay.Client.Bridge;

// The publisher borrows a packet. An accepted queue entry owns precisely one added
// reference; the writer or Dispose frees it. Full queues fail immediately, never blocking
// USB capture or the local target. The participant is then disconnected by its host.
internal sealed class NativeMediaQueue : IDisposable
{
    internal readonly record struct Entry(ReceivedPacket Packet, long PublishedAt);
    readonly Channel<Entry> queue;
    readonly object gate = new();
    int pending;
    bool closed;
    internal int Pending => Math.Max(0, Volatile.Read(ref pending));
    internal NativeMediaQueue(int capacity)
    {
        if (capacity is < 1 or > 128) throw new ArgumentOutOfRangeException(nameof(capacity));
        queue = Channel.CreateBounded<Entry>(new BoundedChannelOptions(capacity)
        { FullMode = BoundedChannelFullMode.Wait, SingleReader = true, SingleWriter = false, AllowSynchronousContinuations = false });
    }
    internal bool TryPublish(ReceivedPacket packet)
    {
        lock (gate)
        {
            if (closed || packet.Buffer is null) return false;
            packet.Buffer.Reference();
            Interlocked.Increment(ref pending);
            if (queue.Writer.TryWrite(new Entry(packet, Stopwatch.GetTimestamp()))) return true;
            Interlocked.Decrement(ref pending);
            packet.Buffer.Free(); return false;
        }
    }
    internal async IAsyncEnumerable<Entry> ReadTimed([EnumeratorCancellation] CancellationToken cancel)
    {
        await foreach (var packet in queue.Reader.ReadAllAsync(cancel))
        {
            Interlocked.Decrement(ref pending);
            yield return packet;
        }
    }
    public void Dispose()
    {
        lock (gate) { closed = true; queue.Writer.TryComplete(); }
        while (queue.Reader.TryRead(out var entry)) { Interlocked.Decrement(ref pending); entry.Packet.Buffer?.Free(); }
    }
}
