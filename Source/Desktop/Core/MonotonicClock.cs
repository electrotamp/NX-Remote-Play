using System;
using System.Diagnostics;

namespace NxRemotePlay.Client.Core;

internal static class MonotonicClock
{
    internal static long MicrosecondsNow => ToMicroseconds(Stopwatch.GetTimestamp(), Stopwatch.Frequency);

    // Widen before multiplying: the raw tick product overflows after days of uptime.
    internal static long ToMicroseconds(long ticks, long frequency) =>
        (long)((Int128)ticks * 1_000_000 / frequency);
}
