using System;
using System.IO;
using System.Threading;

namespace NxRemotePlay.Client.Bridge;

// Per-slot controller delivery log, pad-flow-*.csv in the Diagnostics folder (one file per run of the app, UTC stamps like the
// other flow logs). It records what the desktop actually puts on the USB wire for each virtual-pad slot, so a report such as
// "the phone and the browser share one controller"  can be answered from the data:
//   event rows   capabilities (how many extra pads the console offers), lease (which slot a new viewer was given, or none),
//                release (a slot's pad was detached), lease_end
//   window rows  one per second per slot that did anything: pad packets with a real button/stick state, neutral pad
//                refreshes, HOME, touch, other, and packets the per-viewer gate dropped (neutral before the first real input,
//                touch owned by another viewer, HOME release before attach)
// Never logs packet contents, credentials or addresses. Diagnostics must never throw into the input path.
internal static class PadFlowLog
{
    internal const string Header = "utc,kind,slot,detail,pad_real,pad_neutral,home,touch,other,dropped";
    internal const int Slots = 8;
    static readonly object gate = new();
    static readonly int[] padReal = new int[Slots], padNeutral = new int[Slots], home = new int[Slots], touch = new int[Slots], other = new int[Slots], dropped = new int[Slots];
    static StreamWriter? writer;
    static Timer? timer;
    static bool unavailable;

    static bool Open()
    {
        if (unavailable) return false;
        if (writer is not null) return true;
        string folder = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "NX-Remote-Play", "Diagnostics");
        Directory.CreateDirectory(folder);
        writer = new StreamWriter(Path.Combine(folder, $"pad-flow-{DateTime.UtcNow:yyyyMMdd-HHmmss}-{Guid.NewGuid():N}.csv")) { AutoFlush = true };
        writer.WriteLine(Header);
        timer = new Timer(_ => FlushNow(), null, 1000, 1000);
        return true;
    }

    internal static void Event(string kind, int slot, string detail = "")
    {
        try
        {
            lock (gate)
            {
                if (!Open()) return;
                writer!.WriteLine($"{DateTime.UtcNow:O},{kind},{slot},{detail.Replace(',', ';').Replace('\r', ' ').Replace('\n', ' ')},,,,,,");
            }
        }
        catch (Exception) { unavailable = true; }
    }

    // One packet written to the console for `slot` (packet layouts as in ViewerRegistry.SlotSink / the USB control protocol).
    internal static void Sent(int slot, byte[] packet)
    {
        if (slot < 0 || slot >= Slots) return;
        try
        {
            lock (gate)
            {
                if (!Open()) return;
                if (packet.Length == 17 && packet[0] == 2)
                {
                    bool real = false;
                    for (int i = 1; i < 17 && !real; i++) real = packet[i] != 0;
                    if (real) padReal[slot]++; else padNeutral[slot]++;
                }
                else if (packet.Length == 5 && packet[0] == 3) home[slot]++;
                else if (packet.Length == 5 && packet[0] <= 1) touch[slot]++;
                else other[slot]++;
            }
        }
        catch (Exception) { unavailable = true; }
    }

    // A packet the per-viewer gate refused to forward (it never reached the console).
    internal static void Dropped(int slot)
    {
        if (slot < 0 || slot >= Slots) return;
        lock (gate) dropped[slot]++;
    }

    internal static void FlushNow()
    {
        try
        {
            lock (gate)
            {
                if (writer is null) return;
                string now = DateTime.UtcNow.ToString("O");
                for (int slot = 0; slot < Slots; slot++)
                {
                    if (padReal[slot] + padNeutral[slot] + home[slot] + touch[slot] + other[slot] + dropped[slot] == 0) continue;
                    writer.WriteLine($"{now},window,{slot},,{padReal[slot]},{padNeutral[slot]},{home[slot]},{touch[slot]},{other[slot]},{dropped[slot]}");
                    padReal[slot] = padNeutral[slot] = home[slot] = touch[slot] = other[slot] = dropped[slot] = 0;
                }
            }
        }
        catch (Exception) { unavailable = true; }
    }
}
