using System;
using System.Diagnostics;
using System.IO;
using System.Threading;

namespace NxRemotePlay.Client.Targets.Player;

// Where the desktop viewer's video backlog comes from: counters from the packet intake, the UI thread's
// DecodeFrame calls and the .NET garbage collector, written every 10 s next to the catch-up line in desktop-video.log:
//   "video flow: arrived=<packets> presented=<pictures shown> ui_calls=<UI loop calls> ui_gap_max_ms=<longest gap between UI calls>
//    present_gap_max_ms=<longest gap between pictures> gc0/gc1/gc2=<collections> gc_pause_ms=<total pause>"
// Numbers only. Gaps over 2 s (no stream) are ignored. Measurement only; nothing here changes what is decoded or shown.
internal static class VideoFlowStats
{
    static long arrived, presented, uiCalls, lastCall, lastPresent, maxCallGap, maxPresentGap;
    static int lastGc0, lastGc1, lastGc2;
    static TimeSpan lastPause;
    const long IdleGapMs = 2000;
    // Tests and tools can redirect the log folder; null = the per-user Diagnostics folder.

    internal static void Arrived() => Interlocked.Increment(ref arrived);

    static void NoteGap(ref long last, ref long max)
    {
        long now = Stopwatch.GetTimestamp();
        long before = Interlocked.Exchange(ref last, now);
        if (before == 0) return;
        long gap = now - before;
        if (gap > IdleGapMs * Stopwatch.Frequency / 1000) return;
        long seen;
        while (gap > (seen = Volatile.Read(ref max)) && Interlocked.CompareExchange(ref max, gap, seen) != seen) { }
    }

    internal static void UiCall() { Interlocked.Increment(ref uiCalls); NoteGap(ref lastCall, ref maxCallGap); }
    internal static void Presented() { Interlocked.Increment(ref presented); NoteGap(ref lastPresent, ref maxPresentGap); }

    static double Ms(long ticks) => ticks * 1000.0 / Stopwatch.Frequency;

    // The window text (also resets the counters).
    internal static string TakeWindow()
    {
        long a = Interlocked.Exchange(ref arrived, 0), p = Interlocked.Exchange(ref presented, 0), c = Interlocked.Exchange(ref uiCalls, 0);
        long cg = Interlocked.Exchange(ref maxCallGap, 0), pg = Interlocked.Exchange(ref maxPresentGap, 0);
        int g0 = GC.CollectionCount(0), g1 = GC.CollectionCount(1), g2 = GC.CollectionCount(2);
        TimeSpan pause = GC.GetTotalPauseDuration();
        string text = string.Create(System.Globalization.CultureInfo.InvariantCulture,
            $"video flow: arrived={a} presented={p} ui_calls={c} ui_gap_max_ms={Ms(cg):0} present_gap_max_ms={Ms(pg):0} gc0={g0 - lastGc0} gc1={g1 - lastGc1} gc2={g2 - lastGc2} gc_pause_ms={(pause - lastPause).TotalMilliseconds:0}");
        lastGc0 = g0; lastGc1 = g1; lastGc2 = g2; lastPause = pause;
        return text;
    }

    // Appends one line to desktop-video.log; the file is rotated to desktop-video.prev.log beyond 2 MB.
    internal static void Append(string line)
    {
        try
        {
            string dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "NX-Remote-Play", "Diagnostics");
            Directory.CreateDirectory(dir);
            string path = Path.Combine(dir, "desktop-video.log");
            var info = new FileInfo(path);
            if (info.Exists && info.Length > 2 * 1024 * 1024) File.Move(path, Path.Combine(dir, "desktop-video.prev.log"), true);
            File.AppendAllText(path, $"{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff} {line}{Environment.NewLine}");
        }
        catch (Exception) { }
    }
}
