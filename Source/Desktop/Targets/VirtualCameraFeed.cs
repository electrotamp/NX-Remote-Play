using FFmpeg.AutoGen;
using Microsoft.Win32;
using NxRemotePlay.Client.Targets.Player;
using System;
using System.Diagnostics;
using System.IO;
using System.IO.MemoryMappedFiles;
using System.Security.Cryptography;
using System.Threading;
using System.Threading.Tasks;
using static FFmpeg.AutoGen.ffmpeg;

namespace NxRemotePlay.Client.Targets;

// Desktop side of the "NX Remote Play Camera" (Source\VirtualCamera, NXRemotePlayCam.dll): the decoded Switch picture (the same frame the
// window shows, before rotation, upscaling or any interface) is converted to 1280x720 BGRA and written into the shared memory
// "Local\NXRemotePlayCamera" that the camera DLL reads inside OBS / Discord / Zoom / Chrome. The layout is in nxcam_shared.h.
// Nothing is converted while no program is reading the camera, and the render thread is never blocked: a frame that arrives while the
// converter is busy is simply not offered to the camera. State lines go to desktop-video.log ("virtual camera: ...").
[System.Runtime.Versioning.SupportedOSPlatform("windows")]
internal sealed unsafe class VirtualCameraFeed : IDisposable
{
    const int W = 1280, H = 720, Stride = W * 4, Slots = 3, SlotBytes = Stride * H, HeaderBlock = 4096, MapBytes = HeaderBlock + Slots * SlotBytes;
    const uint Magic = 0x4D43584E, Version = 1;
    const string MapName = "Local\\NXRemotePlayCamera";
    const string ClsidText = "{2A8024F0-9BA8-49A0-92A6-62D999786741}";
    const string DllName = "NXRemotePlayCam.dll";
    // Shipped with the other native files, relative to the exe so the app folder can be moved.
    static string BundledDll => Path.Combine(AppContext.BaseDirectory, "runtimes", "win-x64", "native", DllName);
    const long ReaderFreshMs = 2000;   // a camera program counts as reading while its time stamp is this fresh

    public static VirtualCameraFeed? Instance { get; private set; }
    static readonly object instanceLock = new();

    readonly MemoryMappedFile map;
    readonly MemoryMappedViewAccessor view;
    readonly byte* basePtr;
    readonly Thread worker;
    readonly Timer report;
    readonly AutoResetEvent wake = new(false);
    readonly object gate = new();   // guards staging
    volatile bool stop;
    bool pending;
    AVFrame* staging;
    SwsContext* sws;
    AVFrame* destination;
    long picturesWritten, offered, skippedBusy, lastReportedPictures;
    bool lastReported;
    int lastFormat = -1, lastWidth, lastHeight;

    // ------------------------------------------------------------------------------------------------ lifetime
    public static void Apply(bool enabled)
    {
        lock (instanceLock)
        {
            if (enabled && Instance is null)
            {
                try { Instance = new VirtualCameraFeed(); VideoFlowStats.Append("virtual camera: feed started"); }
                catch (Exception ex) { VideoFlowStats.Append("virtual camera: feed could not start: " + ex.Message); Instance = null; }
            }
            else if (!enabled && Instance is not null) { Instance.Dispose(); Instance = null; VideoFlowStats.Append("virtual camera: feed stopped"); }
        }
    }

    VirtualCameraFeed()
    {
        map = MemoryMappedFile.CreateOrOpen(MapName, MapBytes, MemoryMappedFileAccess.ReadWrite);
        view = map.CreateViewAccessor(0, MapBytes, MemoryMappedFileAccess.ReadWrite);
        byte* p = null;
        view.SafeMemoryMappedViewHandle.AcquirePointer(ref p);
        basePtr = p;
        uint* header = (uint*)basePtr;
        header[0] = Magic; header[1] = Version; header[2] = W; header[3] = H; header[4] = Stride; header[5] = Slots; header[6] = SlotBytes; header[7] = HeaderBlock;
        *(ulong*)(basePtr + 56) = 0;   // no picture yet: the camera shows its "waiting" colour until the first one
        worker = new Thread(Work) { IsBackground = true, Name = "Virtual camera feed" };
        worker.Start();
        report = new Timer(_ => Report(), null, 5000, 5000);
    }

    public void Dispose()
    {
        stop = true;
        report.Dispose();
        wake.Set();
        worker.Join(1000);
        lock (gate)
        {
            if (sws != null) { sws_freeContext(sws); sws = null; }
            fixed (AVFrame** s = &staging) av_frame_free(s);
            if (destination != null) { destination->data[0] = null; fixed (AVFrame** d = &destination) av_frame_free(d); }
        }
        *(ulong*)(basePtr + 56) = 0;   // the camera goes back to "waiting"
        view.SafeMemoryMappedViewHandle.ReleasePointer();
        view.Dispose();
        map.Dispose();
        wake.Dispose();
    }

    // One line when a program starts or stops reading the camera, and one every 5 s while pictures are going out, so a report can show
    // whether the camera was opened, whether frames reached it and in which format.
    void Report()
    {
        try
        {
            bool reading = Reading;
            long written = Volatile.Read(ref picturesWritten);
            if (reading || lastReported || written != lastReportedPictures)
                VideoFlowStats.Append($"virtual camera: program_reading={(reading ? "yes" : "no")} frames_offered={Volatile.Read(ref offered)} pictures_written={written} skipped_busy={Volatile.Read(ref skippedBusy)} source={lastWidth}x{lastHeight} format={(lastFormat < 0 ? "none yet" : ((AVPixelFormat)lastFormat).ToString())}");
            lastReported = reading;
            lastReportedPictures = written;
        }
        catch (Exception) { }
    }

    // ------------------------------------------------------------------------------------------------ frames
    // A camera program is reading while the camera DLL keeps refreshing its time stamp (it does so with every picture it delivers).
    bool Reading
    {
        get
        {
            ulong tick = Volatile.Read(ref *(ulong*)(basePtr + 40));
            return tick != 0 && (ulong)Environment.TickCount64 - tick < ReaderFreshMs;
        }
    }

    // Called on the render thread with the decoded frame. Cheap when nobody reads the camera; never waits.
    public void Offer(AVFrame* frame)
    {
        if (frame == null || frame->width <= 0 || frame->height <= 0 || frame->data[0] == null || !Reading) return;
        Interlocked.Increment(ref offered);
        if (!Monitor.TryEnter(gate)) { Interlocked.Increment(ref skippedBusy); return; }   // the worker is still converting the previous picture
        try
        {
            if (staging == null || staging->format != frame->format || staging->width != frame->width || staging->height != frame->height)
            {
                fixed (AVFrame** s = &staging) av_frame_free(s);
                staging = av_frame_alloc();
                staging->format = frame->format; staging->width = frame->width; staging->height = frame->height;
                if (av_frame_get_buffer(staging, 0) < 0) { fixed (AVFrame** s = &staging) av_frame_free(s); return; }
                lastFormat = frame->format; lastWidth = frame->width; lastHeight = frame->height;
            }
            if (av_frame_copy(staging, frame) < 0) return;
            staging->colorspace = frame->colorspace; staging->color_range = frame->color_range;
            pending = true;
        }
        finally { Monitor.Exit(gate); }
        wake.Set();
    }

    void Work()
    {
        while (!stop)
        {
            wake.WaitOne();
            if (stop) break;
            lock (gate)
            {
                if (!pending || staging == null) continue;
                pending = false;
                try { Convert(); }
                catch (Exception ex) { VideoFlowStats.Append("virtual camera: conversion failed: " + ex.Message); }
            }
        }
    }

    void Convert()
    {
        var format = (AVPixelFormat)staging->format;
        sws = sws_getCachedContext(sws, staging->width, staging->height, format, W, H, AVPixelFormat.AV_PIX_FMT_BGRA, SWS_BILINEAR, null, null, null);
        if (sws == null) { VideoFlowStats.Append($"virtual camera: no converter for {format}"); return; }
        // The window's own conversion treats 720p as BT.709 limited range; keep the camera picture the same colour.
        int srcRange = staging->color_range == AVColorRange.AVCOL_RANGE_JPEG ? 1 : 0;
        var coefficients = sws_getCoefficients(staging->colorspace == AVColorSpace.AVCOL_SPC_BT470BG || staging->colorspace == AVColorSpace.AVCOL_SPC_SMPTE170M ? SWS_CS_ITU601 : SWS_CS_ITU709);
        var table = new int_array4();
        for (uint k = 0; k < 4; k++) table[k] = coefficients[k];
        sws_setColorspaceDetails(sws, table, srcRange, table, 1, 0, 1 << 16, 1 << 16);

        uint latest = Volatile.Read(ref *(uint*)(basePtr + 36));
        uint slot = (latest + 1) % Slots;
        byte* target = basePtr + HeaderBlock + (long)slot * SlotBytes;
        if (destination == null) destination = av_frame_alloc();
        destination->data[0] = target;
        destination->linesize[0] = Stride;
        sws_scale(sws, staging->data, staging->linesize, 0, staging->height, destination->data, destination->linesize);

        Thread.MemoryBarrier();
        Volatile.Write(ref *(uint*)(basePtr + 36), slot);
        Volatile.Write(ref *(ulong*)(basePtr + 48), (ulong)(Interlocked.Increment(ref picturesWritten)));
        Volatile.Write(ref *(ulong*)(basePtr + 56), (ulong)Environment.TickCount64);
    }

    // ------------------------------------------------------------------------------------------------ install / remove (per user, no administrator rights)
    // Every install copies the DLL into its own folder named after the file's hash (Camera\<hash>\NXRemotePlayCam.dll) and registers
    // that path. A browser or camera program that still has an older copy loaded (Chromium browsers, OBS, Discord do that) keeps that
    // one locked, so a fixed path made removing or reinstalling fail; with a folder per version nothing ever has to overwrite or
    // delete a locked file, and old copies are cleaned up whenever they are no longer in use.
    static volatile string note = "";
    public static string Status => note.Length > 0 ? note : IsInstalled ? "Installed" : "Not installed";
    static string? RegisteredPath
    {
        get
        {
            try { using var key = Registry.CurrentUser.OpenSubKey($"Software\\Classes\\CLSID\\{ClsidText}\\InprocServer32"); return key?.GetValue(null) as string; }
            catch { return null; }
        }
    }
    public static bool IsInstalled => RegisteredPath is { } path && File.Exists(path);

    // Programs started "as administrator" (OBS is often set to) ignore per-user COM registrations, so they cannot see a camera that is
    // registered for this user only (Microsoft: COM reads only HKEY_LOCAL_MACHINE\Software\Classes above medium integrity level).
    // This registers the same DLL for all users; it needs an elevated process, so Windows shows its permission (UAC) prompt.
    static string? MachineRegisteredPath
    {
        get
        {
            try { using var key = Registry.LocalMachine.OpenSubKey($"Software\\Classes\\CLSID\\{ClsidText}\\InprocServer32"); return key?.GetValue(null) as string; }
            catch { return null; }
        }
    }
    public static bool IsInstalledAllUsers => MachineRegisteredPath is { } path && File.Exists(path);
    static volatile string adminNote = "";
    public static string AllUsersStatus => adminNote.Length > 0 ? adminNote : IsInstalledAllUsers ? "Installed" : "Not installed";

    public static void InstallAllUsersOrRemoveInBackground()
    {
        if (adminNote == "Working...") return;
        string? current = MachineRegisteredPath;
        bool remove = current is not null;
        adminNote = "Working...";
        if (!remove) { Program.Options.VirtualCamera = true; Apply(true); }
        Task.Run(() =>
        {
            try
            {
                string dll;
                if (remove) dll = current!;
                else
                {
                    string source = BundledDll;
                    if (!File.Exists(source)) { adminNote = "Camera file missing from the app folder"; return; }
                    string folder = Path.Combine(InstallRoot, HashOf(source));
                    dll = Path.Combine(folder, DllName);
                    Directory.CreateDirectory(folder);
                    if (!File.Exists(dll) || HashOf(dll) != HashOf(source)) File.Copy(source, dll, true);
                }
                var start = new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, "regsvr32.exe"), (remove ? "/u /s /n /i:machine \"" : "/s /n /i:machine \"") + dll + "\"") { UseShellExecute = true, Verb = "runas" };
                using var process = Process.Start(start);
                if (process is null) { adminNote = "Could not start the installer"; return; }
                if (!process.WaitForExit(60000)) { adminNote = "Timed out waiting for Windows permission"; return; }
                VideoFlowStats.Append($"virtual camera: all-users {(remove ? "remove" : "install")} regsvr32 exit={process.ExitCode}");
                adminNote = remove ? (MachineRegisteredPath is null ? "" : "Could not remove") : (IsInstalledAllUsers ? "" : "Could not install (needs administrator permission)");
            }
            catch (System.ComponentModel.Win32Exception ex) when (ex.NativeErrorCode == 1223) { adminNote = "Cancelled (permission was not given)"; }
            catch (Exception ex)
            {
                VideoFlowStats.Append("virtual camera: all-users " + (remove ? "remove" : "install") + " failed: " + ex.Message);
                adminNote = remove ? "Could not remove" : "Could not install";
            }
        });
    }
    static string InstallRoot => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "NX-Remote-Play", "Camera");

    public static void InstallOrRemoveInBackground()
    {
        if (note == "Working...") return;
        bool remove = RegisteredPath is not null;   // a leftover registration counts as installed, so one click always cleans it
        note = "Working...";
        // Installing turns the feed on (one step for the user); removing turns it off.
        Program.Options.VirtualCamera = !remove;
        Apply(!remove);
        Task.Run(() =>
        {
            try
            {
                if (remove)
                {
                    string? registered = RegisteredPath;
                    if (registered is not null && File.Exists(registered)) Regsvr(registered, true);
                    if (RegisteredPath is not null) ForgetRegistration();   // the DLL could not unregister itself: remove the keys directly
                    bool leftovers = !CleanOldCopies(null);
                    note = RegisteredPath is not null ? "Could not remove" : leftovers ? "Removed (restart programs that used the camera to free its file)" : "";
                }
                else
                {
                    string source = BundledDll;
                    if (!File.Exists(source)) { note = "Camera file missing from the app folder"; return; }
                    string folder = Path.Combine(InstallRoot, HashOf(source));
                    string target = Path.Combine(folder, DllName);
                    Directory.CreateDirectory(folder);
                    if (!File.Exists(target) || HashOf(target) != HashOf(source)) File.Copy(source, target, true);
                    bool ok = Regsvr(target, false) && string.Equals(RegisteredPath, target, StringComparison.OrdinalIgnoreCase);
                    note = ok ? "" : "Could not install";
                    if (ok) CleanOldCopies(folder);
                    VideoFlowStats.Append("virtual camera: install " + (ok ? "ok " : "FAILED ") + target);
                }
            }
            catch (Exception ex)
            {
                VideoFlowStats.Append("virtual camera: " + (remove ? "remove" : "install") + " failed: " + ex.Message);
                note = remove ? "Could not remove" : "Could not install";
            }
        });
    }

    // Deletes every copy under Camera except 'keep'; true when nothing is left (a locked copy stays and is retried next time).
    public static bool CleanOldCopies(string? keep)
    {
        bool clean = true;
        try
        {
            if (!Directory.Exists(InstallRoot)) return true;
            foreach (var dir in Directory.GetDirectories(InstallRoot))
            {
                if (keep is not null && string.Equals(dir, keep, StringComparison.OrdinalIgnoreCase)) continue;
                if (MachineRegisteredPath is { } all && string.Equals(dir, Path.GetDirectoryName(all), StringComparison.OrdinalIgnoreCase)) continue;   // the all-users registration still points here
                try { Directory.Delete(dir, true); } catch (Exception) { clean = false; }
            }
            if (keep is null) foreach (var file in Directory.GetFiles(InstallRoot))   // the single fixed-path copy older builds made
                {
                    try { File.Delete(file); } catch (Exception) { clean = false; }
                }
            else foreach (var file in Directory.GetFiles(InstallRoot)) { try { File.Delete(file); } catch (Exception) { } }
        }
        catch (Exception) { clean = false; }
        return clean;
    }
    public static void CleanOldCopiesQuietly()
    {
        string? registered = RegisteredPath;
        Task.Run(() => CleanOldCopies(registered is null ? null : Path.GetDirectoryName(registered)));
    }

    static string HashOf(string path)
    {
        using var stream = File.OpenRead(path);
        return System.Convert.ToHexString(SHA256.HashData(stream))[..12];
    }
    static void ForgetRegistration()
    {
        try { Registry.CurrentUser.DeleteSubKeyTree($"Software\\Classes\\CLSID\\{ClsidText}", false); } catch (Exception) { }
        try { Registry.CurrentUser.DeleteSubKeyTree("Software\\Classes\\CLSID\\{860BB310-5D01-11d0-BD3B-00A0C911CE86}\\Instance\\NX Remote Play Camera", false); } catch (Exception) { }
    }
    static bool Regsvr(string dll, bool unregister)
    {
        using var process = Process.Start(new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, "regsvr32.exe"), (unregister ? "/u /s \"" : "/s \"") + dll + "\"") { UseShellExecute = false, CreateNoWindow = true });
        if (process is null) return false;
        if (!process.WaitForExit(15000)) { try { process.Kill(); } catch { } return false; }
        VideoFlowStats.Append($"virtual camera: regsvr32 {(unregister ? "/u " : "")}exit={process.ExitCode}");
        return process.ExitCode == 0;
    }
}
