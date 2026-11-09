using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;

namespace NxRemotePlay.Client.Bridge;

// Controller-path diagnostics, the input-side companion of usb-flow-*.csv (which only sees IN reads).
// input-flow-*.csv has two row kinds: `event` rows for every ownership transition (grant, revoke,
// watchdog_revoke, native_active, native_disconnected...) and one `window` row per second that had
// any controller traffic: USB OUT writes, slowest/failed writes, snapshots dropped because the USB
// writer was busy, and native/local input ignored because a remote owner held the pad.
// Never logs packet contents, participant secrets or credentials. Diagnostics must never throw into input.
internal sealed class InputFlowLog : IDisposable
{
    readonly object gate = new();
    readonly StreamWriter? writer;
    readonly Stopwatch window = Stopwatch.StartNew();
    readonly Dictionary<string, int> totals = new();
    int writes, failures, busyDrops, ignored;
    double maxWriteMs;
    bool disposed;

    // writer == null keeps counters only (used by the synthetic continuity test).
    internal InputFlowLog(StreamWriter? writer = null)
    {
        this.writer = writer;
        Line("utc,kind,name,detail,writes,failures,max_write_ms,busy_drops,ignored");
    }

    internal static InputFlowLog? Open()
    {
        try
        {
            string folder = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "NX-Remote-Play", "Diagnostics");
            Directory.CreateDirectory(folder);
            return new InputFlowLog(new StreamWriter(Path.Combine(folder, $"input-flow-{DateTime.UtcNow:yyyyMMdd-HHmmss}-{Guid.NewGuid():N}.csv")));
        }
        catch (Exception) { return null; }
    }

    internal void Event(string name, string detail = "")
    {
        lock (gate)
        {
            if (disposed) return;
            totals[name] = (totals.TryGetValue(name, out var n) ? n : 0) + 1;
            Line($"{DateTime.UtcNow:O},event,{name},{detail},,,,,");
        }
    }

    internal void Write(double ms, bool failed)
    {
        lock (gate)
        {
            if (disposed) return;
            writes++; if (failed) failures++; maxWriteMs = Math.Max(maxWriteMs, ms);
            Roll();
        }
    }

    internal void BusyDrop() { lock (gate) { if (disposed) return; busyDrops++; Roll(); } }
    internal void Ignored() { lock (gate) { if (disposed) return; ignored++; Roll(); } }
    internal void Tick() { lock (gate) { if (!disposed) Roll(); } }

    void Roll()
    {
        if (window.ElapsedMilliseconds < 1000) return;
        if (writes != 0 || failures != 0 || busyDrops != 0 || ignored != 0)
            Line($"{DateTime.UtcNow:O},window,,,{writes},{failures},{maxWriteMs:F2},{busyDrops},{ignored}");
        writes = failures = busyDrops = ignored = 0; maxWriteMs = 0; window.Restart();
    }

    void Line(string text)
    {
        if (writer is null) return;
        try { writer.WriteLine(text); writer.Flush(); } catch (Exception) { }
    }

    public void Dispose()
    {
        lock (gate)
        {
            if (disposed) return;
            if (writes != 0 || failures != 0 || busyDrops != 0 || ignored != 0)
                Line($"{DateTime.UtcNow:O},window,,,{writes},{failures},{maxWriteMs:F2},{busyDrops},{ignored}");
            disposed = true;
            try { writer?.Dispose(); } catch (Exception) { }
        }
    }
}
