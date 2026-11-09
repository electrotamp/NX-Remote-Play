using System;
using System.IO;

namespace NxRemotePlay.Client.Bridge;

// Browser/WebRTC path diagnostics, the connection-side companion of input-flow-*.csv and usb-flow-*.csv.
// One rtc-flow-*.csv per run of the app in the Diagnostics folder, rows: utc,session,event,detail. Records the router
// mapping results at start, every local and remote ICE candidate (network addresses only) and each connection/ICE state
// change, so a browser that cannot connect (typically from another network) can be explained from the host side.
// Never logs credentials, tickets, SDP bodies or media. Diagnostics must never throw into the connection path.
internal static class RtcFlowLog
{
    static readonly object gate = new();
    static StreamWriter? writer;
    static bool unavailable;

    internal static void Event(string session, string name, string detail = "")
    {
        try
        {
            lock (gate)
            {
                if (unavailable) return;
                if (writer is null)
                {
                    string folder = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "NX-Remote-Play", "Diagnostics");
                    Directory.CreateDirectory(folder);
                    writer = new StreamWriter(Path.Combine(folder, $"rtc-flow-{DateTime.UtcNow:yyyyMMdd-HHmmss}-{Guid.NewGuid():N}.csv")) { AutoFlush = true };
                    writer.WriteLine("utc,session,event,detail");
                }
                string id = session.Length > 8 ? session[..8] : session;
                writer.WriteLine($"{DateTime.UtcNow:O},{id},{name},{detail.Replace(',', ';').Replace('\r', ' ').Replace('\n', ' ')}");
            }
        }
        catch (Exception) { unavailable = true; }
    }
}
