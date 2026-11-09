using System;
using System.Globalization;
using System.IO;
using System.Text;

namespace NxRemotePlay.Client.Bridge;

// Browser-viewer delivery measurements, the companion of rtc-flow-*.csv (connection events). Two files per run of the app in the
// Diagnostics folder, both stamped with the host's UTC clock so they line up with usb-flow-*.csv and the console log:
//   web-host-*.csv    what the desktop did for each browser viewer once a second: media queue depth and the longest time a packet
//                     waited in it, the longest gap between consecutive video frames arriving from the console, the longest time
//                     one frame took to hand to the network, frames/bytes/packets sent.
//   web-browser-*.csv what the browser itself reported once a second (jitter buffer delay, decode time, frames dropped, packets
//                     lost, NACK/PLI counts, freezes, round trip, audio buffer and concealment), received over the viewer's "home"
//                     data channel as "w1,<numbers>" text. Only numbers are accepted; nothing else is written.
// Never logs credentials, tickets, SDP, addresses or media. Diagnostics must never throw into the media path.
internal static class WebFlowLog
{
    internal const string HostHeader = "utc,session,state,pending,max_queue_ms,max_in_gap_ms,max_send_ms,video_aus,idr_aus,skipped_aus,video_bytes,rtp_packets,audio_packets,home_frames,home_buffered";
    internal const string BrowserHeader = "utc,session,t_ms,fps_decoded,fps_received,width,height,jitter_ms,target_ms,decode_ms,ifd_mean_ms,ifd_std_ms,dropped_total,lost_total,rx_packets,rx_kbps,nack,pli,freezes,freeze_ms,rtt_ms,a_jitter_ms,a_conceal,a_accel,a_decel,a_lost_total,hidden,v_min_ms,a_target_ms";
    // The page sends exactly this many numbers after the "w1" tag (empty = unavailable in that browser).
    internal const int BrowserFields = 27;
    internal const int MaxBrowserMessage = 1024;
    static readonly object gate = new();
    static StreamWriter? hostWriter, browserWriter;
    static bool unavailable;

    static StreamWriter Open(string kind, string header)
    {
        string folder = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "NX-Remote-Play", "Diagnostics");
        Directory.CreateDirectory(folder);
        var writer = new StreamWriter(Path.Combine(folder, $"{kind}-{DateTime.UtcNow:yyyyMMdd-HHmmss}-{Guid.NewGuid():N}.csv")) { AutoFlush = true };
        writer.WriteLine(header);
        return writer;
    }
    static string Id(string session) => session.Length > 8 ? session[..8] : session;

    internal static void Host(string session, string fields)
    {
        try
        {
            lock (gate)
            {
                if (unavailable) return;
                hostWriter ??= Open("web-host", HostHeader);
                hostWriter.WriteLine($"{DateTime.UtcNow:O},{Id(session)},{fields}");
            }
        }
        catch (Exception) { unavailable = true; }
    }

    // Validates and records one browser message; false = rejected (wrong tag, wrong field count, anything but numbers).
    internal static bool Browser(string session, ReadOnlySpan<byte> message)
    {
        if (message.Length == 0 || message.Length > MaxBrowserMessage) return false;
        string text;
        try { text = new UTF8Encoding(false, true).GetString(message); } catch (Exception) { return false; }
        var parts = text.Split(',');
        if (parts.Length != BrowserFields + 1 || parts[0] != "w1") return false;
        var line = new StringBuilder();
        for (int i = 1; i < parts.Length; i++)
        {
            if (i > 1) line.Append(',');
            if (parts[i].Length == 0) continue;
            if (parts[i].Length > 24 || !double.TryParse(parts[i], NumberStyles.Float, CultureInfo.InvariantCulture, out double value) || double.IsNaN(value) || double.IsInfinity(value)) return false;
            line.Append(value.ToString("0.##", CultureInfo.InvariantCulture));
        }
        try
        {
            lock (gate)
            {
                if (unavailable) return true;
                browserWriter ??= Open("web-browser", BrowserHeader);
                browserWriter.WriteLine($"{DateTime.UtcNow:O},{Id(session)},{line}");
            }
        }
        catch (Exception) { unavailable = true; }
        return true;
    }
}
