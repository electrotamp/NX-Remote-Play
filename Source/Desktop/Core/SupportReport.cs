using SDL2;
using System;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;

namespace NxRemotePlay.Client.Core
{
    // The desktop half of the "send us a log" tool. One plain text file with what a person helping needs: app version, Windows,
    // graphics mode, settings and the tail of the app's own logs. Log lines are not generally redacted;
    // the report tells users to review it before sharing. The Switch's support file is separate.
    internal static class SupportReport
    {
        static string DiagnosticsDirectory() => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "NX-Remote-Play", "Diagnostics");

        static void Tail(StringBuilder sb, string title, string file, int lines)
        {
            sb.AppendLine().AppendLine("---- " + title + " ----");
            try
            {
                if (!File.Exists(file)) { sb.AppendLine("(none)"); return; }
                foreach (var line in File.ReadLines(file).TakeLast(lines)) sb.AppendLine(line);
            }
            catch (Exception ex) { sb.AppendLine("(could not be read: " + ex.Message + ")"); }
        }

        public static string Build(string? diagnosticsDirectoryOverride = null)
        {
            var o = Program.Options;
            var sb = new StringBuilder();
            sb.AppendLine("== NX-Remote-Play desktop support report ==");
            sb.AppendLine("May contain sensitive diagnostic details, including addresses or identifiers. Review before sharing. The Switch's own logs come from Export logs in the Switch app's Settings (or SD card /config/nxremoteplay/host_support.txt).");
            sb.AppendLine($"Created: {DateTime.Now:yyyy-MM-dd HH:mm:ss zzz}");
            sb.AppendLine($"App: {Program.DisplayVersion}  build {Program.BuildID}");
            sb.AppendLine($"System: {RuntimeInformation.OSDescription} ({RuntimeInformation.OSArchitecture}), .NET {Environment.Version}, {Environment.ProcessorCount} logical CPUs");
            try
            {
                if (Program.SdlCtx is not null)
                {
                    SDL.SDL_GetRendererInfo(Program.SdlCtx.RendererHandle, out var info);
                    sb.AppendLine("Graphics mode: " + Marshal.PtrToStringAnsi(info.name) + (GUI.GlUpscaler.Working ? " (upscaler on)" : ""));
                }
            }
            catch (Exception) { }
            sb.AppendLine($"Upscaler: setting {o.Upscale} (sharpness {o.UpscaleStrength}), state {GUI.GlUpscaler.Status}");
            sb.AppendLine();
            sb.AppendLine("Settings that change behaviour:");
            sb.AppendLine($"  scale mode {o.RendererScale}, software rendering {o.ForceSoftwareRenderer}, decoder {o.DecoderName ?? "Automatic"}, audio mode {o.AudioPlayerMode}");
            sb.AppendLine($"  uncap streaming {o.UncapStreaming}, uncap GUI {o.UncapGUI}, FPS overlay {o.ShowFpsOverlay}, hotkeys {o.PlayerHotkeys}");
            sb.AppendLine($"  USB streaming mode {o.Streaming.Kind}, audio batching {o.Streaming.AudioBatching}, console screen off {o.Streaming.TurnOffConsoleScreen}");
            sb.AppendLine($"  gamepad forwarding {o.DesktopGamepad}, gamepad controls the app {o.ControllerInput}, Guide button {o.GuideButton}");
            sb.AppendLine($"  browser sharing {o.DesktopBrowserSharing}, public sharing {o.DesktopBrowserPublicSharing}, router ports {o.DesktopBrowserRouterMapping}");
            var dir = diagnosticsDirectoryOverride ?? DiagnosticsDirectory();
            Tail(sb, "app errors (last 80 lines)", Path.Combine(dir, "desktop-errors.log"), 80);
            Tail(sb, "video queue (last 40 lines)", Path.Combine(dir, "desktop-video.log"), 40);
            sb.AppendLine().AppendLine("---- diagnostics folder ----");
            try
            {
                foreach (var f in new DirectoryInfo(dir).GetFiles().OrderByDescending(x => x.LastWriteTime).Take(25))
                    sb.AppendLine($"{f.LastWriteTime:yyyy-MM-dd HH:mm}  {f.Length,10:N0} B  {f.Name}");
            }
            catch (Exception) { sb.AppendLine("(none)"); }
            return sb.ToString();
        }

        static readonly string[] FlowKinds = { "usb-flow", "native-flow", "input-flow", "pad-flow", "udpm-flow", "rtc-flow", "web-host", "web-browser" };
        const long MaxFlowFileBytes = 6L * 1024 * 1024, MaxBundleInputBytes = 24L * 1024 * 1024;

        // The text report plus the app's own logs and the two newest flow traces of each kind (timing counters only, no
        // packet contents or credentials) in one zip, so a person sends a single file. Returns the zip's full path.
        public static string SaveBundle(string? diagnosticsDirectoryOverride = null)
        {
            var dir = diagnosticsDirectoryOverride ?? DiagnosticsDirectory();
            Directory.CreateDirectory(dir);
            var path = Path.Combine(dir, $"NX-Remote-Play-desktop-logs-{DateTime.Now:yyyyMMdd-HHmmss}.zip");
            var files = new System.Collections.Generic.List<FileInfo>();
            foreach (var name in new[] { "desktop-errors.log", "desktop-video.log", "desktop-video.prev.log" })
            {
                var f = new FileInfo(Path.Combine(dir, name));
                if (f.Exists) files.Add(f);
            }
            foreach (var kind in FlowKinds)
                files.AddRange(new DirectoryInfo(dir).GetFiles(kind + "-*.csv").OrderByDescending(x => x.LastWriteTimeUtc).Take(2));
            long total = 0;
            using (var zip = ZipFile.Open(path, ZipArchiveMode.Create))
            {
                var report = zip.CreateEntry("support-report.txt");
                using (var w = new StreamWriter(report.Open(), new UTF8Encoding(false))) w.Write(Build(dir));
                foreach (var f in files)
                {
                    if (f.Length > MaxFlowFileBytes || total + f.Length > MaxBundleInputBytes) continue;
                    total += f.Length;
                    try { zip.CreateEntryFromFile(f.FullName, f.Name, CompressionLevel.Optimal); }
                    catch (IOException) { } // a trace still being written is skipped, not fatal
                }
            }
            return path;
        }
    }
}
