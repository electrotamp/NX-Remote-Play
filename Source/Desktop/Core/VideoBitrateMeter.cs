using NxRemotePlay.Client.Sources;
using System;
using System.Diagnostics;

namespace NxRemotePlay.Client.Core;

// Measures encoded H.264 payload at the USB reader, before native or browser transport.
internal sealed class VideoBitrateMeter
{
    readonly object gate = new();
    long firstTick, windowTick, totalBytes, windowBytes, windowFrames;
    double recentMbps, recentFps;

    internal void Observe(ReceivedPacket packet)
    {
        if (!packet.Header.IsVideo || packet.Header.IsScreenshot || packet.Header.IsError || packet.Buffer is null) return;
        long now = Stopwatch.GetTimestamp();
        lock (gate)
        {
            if (firstTick == 0) firstTick = windowTick = now;
            long elapsed = now - windowTick;
            if (elapsed >= Stopwatch.Frequency)
            {
                recentMbps = windowBytes * 8.0 / 1_000_000 * Stopwatch.Frequency / elapsed;
                recentFps = windowFrames * (double)Stopwatch.Frequency / elapsed;
                windowBytes = 0; windowFrames = 0; windowTick = now;
            }
            windowFrames++;
            totalBytes += packet.Buffer.Length;
            windowBytes += packet.Buffer.Length;
        }
    }

    // Video packets (one per access unit) arriving from the Switch per second: what the console actually delivers,
    // before decoding, native or browser transport.
    internal double ReceivedFps()
    {
        long now = Stopwatch.GetTimestamp();
        lock (gate) return firstTick == 0 || (now - windowTick) / (double)Stopwatch.Frequency > 2 ? 0 : recentFps;
    }

    internal (double RecentMbps, double AverageMbps, double Seconds) Snapshot()
    {
        long now = Stopwatch.GetTimestamp();
        lock (gate)
        {
            if (firstTick == 0) return (0, 0, 0);
            double seconds = (now - firstTick) / (double)Stopwatch.Frequency;
            double recent = (now - windowTick) / (double)Stopwatch.Frequency > 2 ? 0 : recentMbps;
            return (recent, totalBytes * 8.0 / 1_000_000 / seconds, seconds);
        }
    }
}
