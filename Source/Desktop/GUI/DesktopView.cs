using ImGuiNET;
using NxRemotePlay.Client.App;
using NxRemotePlay.Client.Core;
using NxRemotePlay.Client.GUI.Components;
using NxRemotePlay.Client.Sources;
using NxRemotePlay.Client.Targets.Player;
using NxRemotePlay.Client.Targets.FileOutput;
using NxRemotePlay.Client.Targets;
using NxRemotePlay.Client.Platform;
using NxRemotePlay.Client.Platform.Specific.Win;
using NxRemotePlay.Client.Bridge;
using SIPSorcery.Net;
using System.Collections.Generic;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System;
using System.IO;
using System.Numerics;
using System.Threading;
using System.Threading.Tasks;

namespace NxRemotePlay.Client.GUI;

// Desktop interface; capture and decoding are handled by the shared player.
internal sealed partial class DesktopView : View
{
    public DesktopView(ClientApp owner) : base(owner) { RenderMode = Program.Options.UncapStreaming ? FramerateCapOptions.Uncapped() : FramerateCapOptions.Target(60); }
    enum Page { Home, Join, Settings }
    Page page;
    string id = Program.Options.LastRelayId, password = Program.Options.LastRelayPassword ?? "", server = NxRemotePlay.Client.Core.ServerAddress.Effective(Program.Options);
    string status = "Ready", error = "";
    string usbError = "";
    bool usbUnavailable;
    Image? logo, joycon;
    PlayerCore? player;
    CancellationTokenSource? session;
    Task<PlayerManager>? connecting;
    Task<DvrUsbDevice?>? scanning;
    Task<string>? driverInstall;
    bool driverTried, driverNoted;
    readonly CancellationTokenSource lifetime = new();
    DvrUsbContext? usb;
    PunchSource? punch;
    PunchController? controller;
    UsbStreamingSource? usbSource;
    VideoBitrateMeter? usbVideoMeter;
    DesktopBridgeSession? bridge;
    Task<DesktopBridgeSession>? bridgeStarting;
    DesktopBrowserSession? browser;
    Task<DesktopBrowserSession>? browserStarting;
    CancellationTokenSource? bridgeLifetime;
    Mp4Output? recorder;
    // hideText hides EVERYTHING drawn over the stream (status, links, bitrate, menu, footer, errors) except its own
    // always-visible toggle button; also toggled with the H key.
    bool streamMenu, hideText;
    int rotation;
    bool usbSession, autoUsb = Program.Options.DesktopAutoUsb;
    // After the user ends a USB session on purpose the app must not reopen it by itself: it waits for the USB Switch button, or for the
    // Switch to be unplugged and plugged in again. usbManual = the button was pressed: open a plugged-in Switch even if automatic opening is off.
    bool usbHold, usbHoldUnplugged, usbManual;
   // render checks only: click Disconnect on the first frame
    bool appControlOn;     // the Guide button gave the controller to the interface
    bool wasFullscreen;    // full screen hides the interface, leaving it brings it back
    int drawFrame, settingsLastFrame = -10;
    enum PadZone { Tabs, Items }
    PadZone padZone;       // Settings with a controller: the category list on the left, or the settings of the chosen category
    int padItem;
    bool padScroll;
    bool modalOpen;        // a text box / on-screen keyboard popup is open: it uses ImGui navigation
    List<Item>? lastItems;
    float[] lastHeights = Array.Empty<float>();
    string editLabel = "", editValue = "";
    Action<string>? applyEdit;
    bool openEditPopup;   // set by a row; the popup is opened after the list so OpenPopup and BeginPopupModal share one ID scope
    double nextScan;
    int generation;
    float scale;
    Vector2 origin;
    ImDrawListPtr draw;
    static readonly uint White = Color(1, 1, 1), Muted = Color(.70f, .76f, .88f);
    static uint Color(float r, float g, float b, float a = 1) => ImGui.ColorConvertFloat4ToU32(new(r, g, b, a));
    Vector2 At(float x, float y) => origin + new Vector2(x, y) * scale;

    public override void Created()
    {
        unsafe { ImGui.GetIO().NativePtr->FontDefault = Owner.DesktopFont.NativePtr; }
        SDL2.SDL.SDL_InitSubSystem(SDL2.SDL.SDL_INIT_GAMECONTROLLER);
        string assets = Path.Combine(AppContext.BaseDirectory, "runtimes", "Desktop");
        logo = Image.FromFile(Owner, Path.Combine(assets, "app_logo.png"));
        joycon = Image.FromFile(Owner, Path.Combine(assets, "connect_joycon_wireless.png"));
        Owner.OnExit += CancelLifetime;
            UpdateCheck.Start();
    }
    void CancelLifetime() { Disconnect(); lifetime.Cancel(); }
    public override void ResolutionChanged() => player?.ResolutionChanged();
    // The user ended the connection (Disconnect, Cancel, Esc): end it and stay ended. A USB session is not reopened by itself afterwards.
    void UserDisconnect()
    {
        bool wasUsb = usbSession;
        streamMenu = false;
        Disconnect();
        if (!wasUsb) return;
        usbHold = true; usbHoldUnplugged = false; usbManual = false;
        Notify("Disconnected", "Press USB Switch to connect again", AccentBlue, 6, group: "usb");
    }
    // The USB Switch button on the first page: open a plugged-in Switch now (also when automatic opening is off).
    void OpenUsbByHand()
    {
        usbHold = false; usbHoldUnplugged = false; usbUnavailable = false; usbManual = true; driverTried = false;
        usbError = ""; error = ""; nextScan = 0;
        status = "Looking for a USB Switch...";
    }
    bool SettingsVisible => connecting is null && page == Page.Settings && (player is null || streamSettings);
    public override bool WantsPadActions => SettingsVisible && !modalOpen;
    void LeaveSettings() { page = Page.Home; streamSettings = false; error = ""; }
    void ActivateItem(Item item)
    {
        if (item.Get is not null) item.Set!(!item.Get()); else item.Activate?.Invoke();
        SaveSettings();
    }
    // Settings works like the Switch's own system settings: the category list on the left is one level, the settings of the chosen
    // category the next. A or Right goes in, B comes back to the list, B on the list leaves Settings.
    public override void OnPadAction(PadAction action)
    {
        if (!SettingsVisible) return;
        int count = lastItems?.Count ?? 0;
        if (padZone == PadZone.Tabs)
        {
            switch (action)
            {
                case PadAction.Up: if (tab > 0) { tab--; tabChanged = true; } break;
                case PadAction.Down: if (tab < TabNames.Length - 1) { tab++; tabChanged = true; } break;
                case PadAction.Right: case PadAction.Accept: if (count > 0) { padZone = PadZone.Items; padItem = 0; padScroll = true; } break;
                case PadAction.Back: LeaveSettings(); break;
            }
            return;
        }
        if (count == 0) { padZone = PadZone.Tabs; return; }
        padItem = Math.Clamp(padItem, 0, count - 1);
        var item = lastItems![padItem];
        switch (action)
        {
            case PadAction.Up: if (padItem > 0) { padItem--; padScroll = true; } break;
            case PadAction.Down: if (padItem < count - 1) { padItem++; padScroll = true; } break;
            case PadAction.Left: if (item.Step is not null) { item.Step(-1); SaveSettings(); } else padZone = PadZone.Tabs; break;
            case PadAction.Right: if (item.Step is not null) { item.Step(1); SaveSettings(); } break;
            case PadAction.Accept: ActivateItem(item); break;
            case PadAction.Back: padZone = PadZone.Tabs; break;
        }
    }
    // Per-client "disconnect controller": off = this computer's gamepad is not forwarded and this viewer holds no player place.
    void ToggleControllerForwarding()
    {
        Program.Options.DesktopGamepad = !Program.Options.DesktopGamepad;
        Program.ControllerInputForwardingActive = Program.Options.DesktopGamepad && !appControlOn;
        Owner.ApplyImguiSettings();
        SaveSettings();
        Notify(Program.Options.DesktopGamepad ? "Controller connected" : "Controller disconnected", Program.Options.DesktopGamepad ? "This computer's gamepad plays on the Switch again" : "This viewer no longer takes a player place", Program.Options.DesktopGamepad ? Good : AccentBlue, 3, group: "pad");
    }
    // The controller's Guide button handed the controller to this interface (or took it back for the Switch).
    void SetAppControl(bool on, bool announce)
    {
        appControlOn = on;
        Program.AppControlActive = on;
        Program.ControllerInputForwardingActive = Program.Options.DesktopGamepad && !on;
        Owner.ApplyImguiSettings();
        if (announce)
            Notify(on ? "Controller: moving the app" : "Controller: playing on the Switch", on ? "Press Guide again to go back to the Switch" : "", on ? AccentBlue : Good, 3, group: "pad", top: true);
    }
    // Esc / the controller's back button leave full screen. They never end a stream: only the Disconnect button does that.
    public override void BackPressed()
    {
        if (Program.SdlCtx.IsFullscreen) { Program.SdlCtx.SetFullScreen(false); return; }
        if (player is not null) { if (streamSettings) { streamSettings = false; page = Page.Home; } return; }
        if (connecting is not null) UserDisconnect();
        else page = Page.Home;
    }
    public override void Destroy()
    {
        Disconnect();
        lifetime.Cancel();
        // All pending results must be reclaimed even if the window closes mid-connection.
        if (scanning is not null)
        {
            try { scanning.GetAwaiter().GetResult()?.Dispose(); } catch { }
            scanning = null;
        }
        logo?.Dispose(); joycon?.Dispose();
        Owner.OnExit -= CancelLifetime;
        unsafe { ImGui.GetIO().NativePtr->FontDefault = Owner.FontText.NativePtr; }
        lifetime.Dispose();
    }
    // Ending a connection must never hold the window for more than this, however the far side behaves.
    // Anything still unwinding after the budget finishes in the background (its tokens are already cancelled).
    const int DisconnectBudgetMilliseconds = 3000;
    internal static void WithinBudget(Task task, System.Diagnostics.Stopwatch budget, int totalMilliseconds = DisconnectBudgetMilliseconds)
    {
        int left = totalMilliseconds - (int)budget.ElapsedMilliseconds;
        try { if (left > 0) task.Wait(left); } catch (Exception) { }
    }
    static Task DisposeWhenStarted<T>(Task<T> starting) where T : IAsyncDisposable =>
        starting.ContinueWith(t => t.IsCompletedSuccessfully ? t.Result.DisposeAsync().AsTask() : Task.CompletedTask, TaskScheduler.Default).Unwrap();

    void Disconnect()
    {
        var budget = System.Diagnostics.Stopwatch.StartNew();
        generation++;
        if (usbVideoMeter is not null && player is not null) player.Manager.OnMediaPacket -= usbVideoMeter.Observe;
        usbVideoMeter = null;
        controller?.Dispose(); controller = null;
        appControlOn = false; Program.AppControlActive = false;
        bridgeLifetime?.Cancel();
        if (browserStarting is not null)
        {
            WithinBudget(DisposeWhenStarted(browserStarting), budget);
            browserStarting = null;
        }
        if (browser is not null)
        {
            WithinBudget(browser.DisposeAsync().AsTask(), budget);
            browser = null;
        }
        if (bridgeStarting is not null)
        {
            WithinBudget(DisposeWhenStarted(bridgeStarting), budget);
            bridgeStarting = null;
        }
        if (bridge is not null)
        {
            WithinBudget(bridge.DisposeAsync().AsTask(), budget);
            bridge = null;
        }
        bridgeLifetime?.Dispose(); bridgeLifetime = null;
        session?.Cancel();
        punch?.Dispose(); usbSource = null;
        Program.ControllerInputForwardingActive = false;
        Owner.ApplyImguiSettings();
        if (connecting is not null)
        {
            WithinBudget(connecting.ContinueWith(t => { if (t.IsCompletedSuccessfully) try { t.Result.Dispose(); } catch (Exception) { } }, TaskScheduler.Default), budget);
            connecting = null;
        }
        if (recorder is not null && player is not null)
        {
            try { StopRecording(); } catch (Exception ex) { error = "Recording could not finish: " + ex.Message; }
        }
        player?.Destroy(Math.Max(0, DisconnectBudgetMilliseconds - (int)budget.ElapsedMilliseconds)); player = null;
        session?.Dispose(); session = null; punch = null;
        status = "Ready";
        rotation = 0; streamMenu = false;
        nextScan = ImGui.GetTime() + 2;
    }
    void BeginConnection(Func<CancellationTokenSource, Task<PlayerManager>> connect, bool viaUsb)
    {
        error = ""; usbSession = viaUsb;
        session = CancellationTokenSource.CreateLinkedTokenSource(lifetime.Token);
        session.CancelAfter(65000);
        status = viaUsb ? "Opening the USB Switch..." : "Connecting...";
        if (viaUsb) Notify("USB Switch detected", "Opening it...", AccentBlue, 3, group: "usb");
        connecting = Task.Run(() => connect(session));
    }
    void Join()
    {
        try
        {
            id = id.Trim().ToUpperInvariant(); password = password.Trim().ToUpperInvariant();
            PunchSource.ValidateCredentials(id, password);
            if (Uri.TryCreate(server.Trim(), UriKind.Absolute, out var secure) && secure.Scheme == "https")
            {
                Program.Options.LastRelayServer = server; Program.Options.LastRelayId = id; Program.Options.LastRelayPassword = password;
                SystemUtil.StoreSettingsString(Program.Options.SerializeToJson());
                int secureGeneration = generation;
                BeginConnection(async token =>
                {
                    punch = await BridgeJoinConnector.Connect(secure, id, password,
                        new StreamingOptions { Kind = StreamKind.Both, AudioBatching = 1, UseNALReplay = false }, token.Token,
                        message: text => Owner.PostAction(() => { if (generation == secureGeneration && connecting is not null) status = text; }));
                    return new PlayerManager(punch, token);
                }, false);
                return;
            }
            if (!NxRemotePlay.Client.Core.ServerAddress.TryParse(server, out var serverHost, out int port))
                throw new ArgumentException("Set the server in Settings: an IP address or a name, with an optional :port.");
            var address = new[] { serverHost, port.ToString() };
            Program.Options.LastRelayServer = server;
            Program.Options.LastRelayId = id;
            Program.Options.LastRelayPassword = password;
            SystemUtil.StoreSettingsString(Program.Options.SerializeToJson());
            int current = generation;
            BeginConnection(async token =>
            {
                var source = new PunchSource(address[0], port, id, password,
                    new StreamingOptions { Kind = StreamKind.Both, AudioBatching = 1, UseNALReplay = false, UseNALReplayOnlyOnKeyframes = false }, token.Token);
                punch = source;
                source.OnMessage += message => Owner.PostAction(() => { if (generation == current && connecting is not null) status = message; });
                await source.Connect();
                return new PlayerManager(source, token);
            }, false);
        }
        catch (Exception ex) { error = ex.Message; }
    }
    void Poll()
    {
        if (player is not null && !usbSession && punch is not null) status = punch.Route;
        if (connecting?.IsCompleted == true)
        {
            var result = connecting; connecting = null;
            try
            {
                var manager = result.GetAwaiter().GetResult();
                if (session!.IsCancellationRequested) { manager.Dispose(); session.Token.ThrowIfCancellationRequested(); }
                session!.CancelAfter(Timeout.Infinite);
                int current = generation;
                void Failed(string text) => Owner.PostAction(() =>
                {
                    if (generation != current || player is null) return;
                    bool wasUsb = usbSession;
                    Disconnect();
                    if (wasUsb)
                    {
                        error = "";
                        Notify("USB Switch disconnected", autoUsb ? "It reconnects when it is plugged in again" : "Press USB Switch to connect again", Bad, 8, group: "usb");
                    }
                    else
                    {
                        error = text;
                        var (ft, fd) = SplitNotice(text);
                        Notify("Connection ended", fd.Length > 0 ? fd : ft, Bad, 8);
                    }
                });
                manager.OnFatalError += ex => Failed("The connection ended. " + ex.Message);
                if (usbSession)
                {
                    // StreamManager reports a failed USB read before calling Source.Flush(),
                    // which owns the reconnect attempt. Treating that warning as fatal here
                    // cancelled the attempt before the device could be reopened.
                    int recoveryPending = 0;
                    manager.OnErrorMessage += message =>
                    {
                        bool reconnecting = message.StartsWith("Error reading next packet:", StringComparison.Ordinal)
                            || message.StartsWith("USB warning:", StringComparison.Ordinal);
                        if (reconnecting) Interlocked.Exchange(ref recoveryPending, 1);
                        Owner.PostAction(() =>
                        {
                            if (generation != current || player is null) return;
                            if (reconnecting) { status = "USB Viewer: reconnecting"; Notify("USB Switch disconnected", "Trying to reconnect...", Warn, 6, group: "usb", top: true); }
                            else error = message;
                        });
                    };
                    manager.OnMediaPacket += packet =>
                    {
                        if (packet.Header.IsError || Interlocked.Exchange(ref recoveryPending, 0) != 1) return;
                        Owner.PostAction(() =>
                        {
                            if (generation != current || player is null) return;
                            error = "";
                            Notify("USB recovered", "Streaming again", Good, 3, group: "usb", top: true);
                            status = bridge?.Online == true ? "Bridge Mode • " + usbSource!.SessionId
                                : browser is not null ? "USB • Browser sharing ready" : "USB Viewer";
                        });
                    };
                }
                if (usbSession) { usbVideoMeter = new VideoBitrateMeter(); manager.OnMediaPacket += usbVideoMeter.Observe; }
                try { player = new PlayerCore(manager, false); player.ResolutionChanged(); player.Start(); }
                catch { manager.Dispose(); throw; }
                status = usbSession ? "USB Viewer" : punch!.Route;
                if (usbSession) Notify("USB Switch connected", "Streaming over USB", Good, 3, group: "usb", top: true);
                if (usbSession) bridgeLifetime = CancellationTokenSource.CreateLinkedTokenSource(session.Token);
                if (usbSession && usbSource is { } source && (source.SupportsDelegation || source.CanEnroll))
                {
                    var token = bridgeLifetime.Token;
                    status = source.CanEnroll ? "USB Viewer: setting up bridge ownership (first time)" : "USB Viewer: connecting bridge";
                    bridgeStarting = Task.Run(async () =>
                    {
                        // A console with no owner key yet gets one automatically the first time it is plugged in.
                        if (source.CanEnroll)
                            await OwnerKeyEnrollment.Run(source, server, token, text => Owner.PostAction(() => { if (generation == current && player is not null) status = text; }));
                        var destination = Uri.TryCreate(server.Trim(), UriKind.Absolute, out var secure) && secure.Scheme == "https" ? secure : NativeOwnerClient.ParseAddress(server);
                        var addresses = await Dns.GetHostAddressesAsync(destination.Host, AddressFamily.InterNetwork, token);
                        if (addresses.Length == 0) throw new Exception("Bridge server has no IPv4 address.");
                        using var routeSocket = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
                        routeSocket.Connect(addresses[0], destination.Port);
                        var address = ((IPEndPoint)routeSocket.LocalEndPoint!).Address;
                        var next = new DesktopBridgeSession(source, manager, server, address, new RTCConfiguration(), token);
                        next.Status += text => Owner.PostAction(() => { if (generation == current && player is not null) status = text; });
                        return next;
                    }, token);
                }
                else if (usbSession) error = "Native hosting unavailable: update the Switch firmware to enable automatic USB hosting.";
                if (manager.InputSink is { SupportsInput: true } input)
                {
                    controller = new PunchController(input);
                    Program.ControllerInputForwardingActive = Program.Options.DesktopGamepad;
                    Owner.ApplyImguiSettings();
                }
                if (usbSession && bridgeStarting is null) StartBrowser(manager);
            }
            catch (Exception ex) { Disconnect(); error = ex is OperationCanceledException ? "Connection cancelled or timed out." : ex.Message; }
        }
        if (bridgeStarting?.IsCompleted == true)
        {
            var started = bridgeStarting; bridgeStarting = null;
            try
            {
                bridge = started.GetAwaiter().GetResult();
                controller?.Dispose(); controller = new PunchController(bridge.Input);
                status = bridge.Online ? "Bridge Mode • " + usbSource!.SessionId : "USB Viewer: connecting bridge";
            }
            catch (Exception ex) { status = "USB Viewer"; error = "Bridge unavailable: " + ex.Message; }
            if (player is not null) StartBrowser(player.Manager);
        }
        if (browserStarting?.IsCompleted == true)
        {
            var started = browserStarting; browserStarting = null;
            try
            {
                browser = started.GetAwaiter().GetResult();
                controller?.Dispose(); controller = new PunchController(browser.Input);
                status = bridge?.Online == true ? "Bridge Mode • " + usbSource!.SessionId : "USB • Browser sharing ready";
            }
            catch (Exception ex) { error = "Browser sharing unavailable: " + ex.Message; }
        }
        if (browser is not null && !browser.Active)
        {
            _ = browser.DisposeAsync().AsTask().ContinueWith(t => _ = t.Exception, TaskScheduler.Default); browser = null; // never block the UI thread on the shutdown (three-second rule)
            controller?.Dispose(); controller = bridge is not null ? new PunchController(bridge.Input) : usbSource is not null ? new PunchController(usbSource) : null;
            Notify("Browser sharing stopped", "Reconnect USB to restart sharing", Warn, 6, group: "usb", top: true);
        }
        if (scanning?.IsCompleted == true)
        {
            var result = scanning; scanning = null;
            try
            {
                var device = result.GetAwaiter().GetResult();
                bool busy = usbSession && (player is not null || connecting is not null);
                if (device is not null)
                {
                    bool open = !busy && (usbManual || autoUsb && (!usbHold || usbHoldUnplugged));
                    if (!open) { device.Dispose(); if (busy) usbManual = false; }
                    else
                    {
                        usbHold = false; usbHoldUnplugged = false; usbManual = false;
                        if (player is not null || connecting is not null) Disconnect();
                        BeginConnection(async token =>
                        {
                            var source = new UsbStreamingSource(device, Program.Options.Streaming.Clone(), token.Token, negotiateControls: true, negotiateOwner: true, failFast: true);
                            try { await source.Connect(); usbSource = source; return new PlayerManager(source, token); }
                            catch { source.Dispose(); throw; }
                        }, true);
                    }
                }
                else
                {
                    if (usbHold) usbHoldUnplugged = true;
                    if (usbManual)
                    {
                        usbManual = false;
                        if (connecting is null && player is null) status = "Ready";
                        Notify("No USB Switch found", "Plug it in with its USB cable, then press USB Switch again", Warn, 6, group: "usb");
                    }
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine("USB detection failed: " + ex);
                usbError = "USB unavailable. Check that the complete app folder is present.";
                Notify("USB unavailable", "Check that the complete app folder is present", Warn, 8, group: "usb");
                usbUnavailable = true;
            }
            nextScan = ImGui.GetTime() + 2;
        }
        if (driverInstall is not null)
        {
            if (!driverNoted) { driverNoted = true; Notify("Installing USB driver", "Windows will ask for permission to install it", Warn, 10, group: "usb"); }
            if (driverInstall.IsCompleted)
            {
                var finished = driverInstall; driverInstall = null; driverNoted = false; nextScan = 0;
                string note = finished.IsCompletedSuccessfully ? finished.Result : "USB driver install failed: " + (finished.Exception?.GetBaseException().Message ?? "unknown error");
                Console.WriteLine(note);
                Notify("USB driver", note, Warn, 10, group: "usb");
            }
        }
        if ((autoUsb || usbHold || usbManual) && !usbUnavailable && driverInstall is null && !(usbSession && (player is not null || connecting is not null)) && scanning is null && ImGui.GetTime() >= nextScan)
            scanning = Task.Run(() =>
            {
                usb ??= new DvrUsbContext(UsbLogLevel.None);
                using var devices = usb.FindNxRemotePlayDevices();
                if (devices.Count == 0)
                {
                    // A Switch that Windows can see but has no driver for never reaches libusb: install the driver once.
                    if (!driverTried && WinUsbDriver.GetState() == UsbDriverState.Missing)
                    {
                        driverTried = true;
                        driverInstall = WinUsbDriver.InstallAsync(lifetime.Token);
                    }
                    return null;
                }
                var selected = devices[0]; devices.ExcludeFromDispose(selected); return selected;
            }, lifetime.Token);
    }

    void StartBrowser(StreamManager manager)
    {
        if (!Program.Options.DesktopBrowserSharing || usbSource is null || browserStarting is not null || browser is not null) return;
        browserStarting = DesktopBrowserSession.Start(usbSource, manager, Program.Options.DesktopBrowserPort,
            bridgeLifetime!.Token, bridge?.Input, server, Program.Options.DesktopBrowserPublicSharing,
            Program.Options.DesktopBrowserRouterMapping, Program.Options.DesktopBrowserMediaPort, Program.Options.DesktopBrowserCertificatePath,
            Program.Options.DesktopBrowserAutomaticCertificate, Program.Options.DesktopBrowserCertificateAgreementAccepted, bridge?.Viewers);
    }
    void Text(string text, float x, float y, float size, bool bold = false, uint? color = null)
    {
        if (string.IsNullOrEmpty(text)) return;
        draw.AddText(bold ? Owner.DesktopBold : Owner.DesktopFont, size * scale, At(x, y), color ?? White, text);
    }
    Vector2 Measure(string text, float size, bool bold = false)
    {
        if (string.IsNullOrEmpty(text)) return Vector2.Zero;
        ImGui.PushFont(bold ? Owner.DesktopBold : Owner.DesktopFont);
        var measured = ImGui.CalcTextSize(text) * (size / ImGui.GetFontSize());
        ImGui.PopFont(); return measured;
    }
    void CenterText(string text, float x, float y, float width, float size, bool bold = false, uint? color = null) =>
        Text(text, x + (width - Measure(text, size, bold).X) / 2, y, size, bold, color);
    void CardGradient(float left, bool usbCard)
    {
        float ox = left - 470;
        var points = new Vector2[4 * 65];
        var centers = new Vector2[] { new(498 + ox, 238), new(782 + ox, 238), new(782 + ox, 532), new(498 + ox, 532) };
        int count = 0;
        for (int corner = 0; corner < 4; corner++)
            for (int i = 0; i <= 64; i++)
            {
                float angle = MathF.PI + corner * MathF.PI / 2 + i * MathF.PI / 128;
                points[count++] = centers[corner] + new Vector2(MathF.Cos(angle), MathF.Sin(angle)) * 28;
            }
        uint Blend(Vector2 p)
        {
            float x = (p.X - 470 - ox) / 340, y = (p.Y - 210) / 350;
            var top = usbCard ? Vector4.Lerp(new(.05f, .58f, .62f, 1), new(.07f, .50f, .68f, 1), x) : Vector4.Lerp(new(.10f, .57f, .98f, 1), new(.12f, .50f, .97f, 1), x);
            var bottom = usbCard ? Vector4.Lerp(new(.08f, .36f, .58f, 1), new(.12f, .26f, .64f, 1), x) : Vector4.Lerp(new(.14f, .32f, .88f, 1), new(.18f, .20f, .78f, 1), x);
            return ImGui.ColorConvertFloat4ToU32(Vector4.Lerp(top, bottom, y));
        }
        draw.PrimReserve(count * 3, count + 1);
        uint centerIndex = draw._VtxCurrentIdx;
        var uv = ImGui.GetIO().Fonts.TexUvWhitePixel;
        draw.PrimWriteVtx(At(640 + ox, 385), uv, Blend(new(640 + ox, 385)));
        foreach (var p in points) draw.PrimWriteVtx(At(p.X, p.Y), uv, Blend(p));
        for (int i = 0; i < count; i++)
        {
            draw.PrimWriteIdx((ushort)centerIndex); draw.PrimWriteIdx((ushort)(centerIndex + 1 + i));
            draw.PrimWriteIdx((ushort)(centerIndex + 1 + (i + 1) % count));
        }
    }
    bool Button(string label, float x, float y, float width, float height, float fontSize = 32, bool blue = false, bool primary = false)
    {
        ImGui.SetCursorScreenPos(At(x, y));
        bool pressed = ImGui.InvisibleButton(label, new Vector2(width, height) * scale);
        bool hovered = ImGui.IsItemHovered();
        if (primary) ImGui.SetItemDefaultFocus();   // the page's primary control starts focused
        if (ImGui.IsItemFocused()) draw.AddRect(At(x - 3, y - 3), At(x + width + 3, y + height + 3), Color(.42f, .76f, 1), 27 * scale, ImDrawFlags.RoundCornersAll, 3 * scale);
        draw.AddRectFilled(At(x, y), At(x + width, y + height), blue ? Color(.12f, hovered ? .49f : .39f, .91f) : Color(.17f, .21f, hovered ? .36f : .30f), 24 * scale);
        var font = Owner.DesktopBold;
        ImGui.PushFont(font);
        var measure = ImGui.CalcTextSize(label) * (fontSize * scale / ImGui.GetFontSize());
        ImGui.PopFont();
        draw.AddText(font, fontSize * scale, At(x + width / 2, y + height / 2) - measure / 2, White, label);
        return pressed;
    }
    public override void Draw()
    {
        Poll();
        drawFrame++;
        // Full screen is full screen: no interface until it is asked for (H, or the Guide button in app mode); leaving it brings the interface back.
        if (player is not null && Program.SdlCtx.IsFullscreen != wasFullscreen) { wasFullscreen = Program.SdlCtx.IsFullscreen; hideText = wasFullscreen; }
        var size = ImGui.GetIO().DisplaySize;
        scale = Math.Min(size.X / 1280, size.Y / 720);
        origin = (size - new Vector2(1280, 720) * scale) / 2;
        ImGui.SetNextWindowPos(Vector2.Zero); ImGui.SetNextWindowSize(size);
        ImGui.PushStyleColor(ImGuiCol.WindowBg, new Vector4(0, 0, 0, 0));
        ImGui.PushStyleVar(ImGuiStyleVar.WindowBorderSize, 0);
        ImGui.Begin("NX Remote Play", ImGuiWindowFlags.NoDecoration | ImGuiWindowFlags.NoMove | ImGuiWindowFlags.NoSavedSettings | ImGuiWindowFlags.NoScrollbar);
        ImGui.PopStyleVar();
        ImGui.PopStyleColor(); draw = ImGui.GetWindowDrawList();
        PollFolderPick();
        if (player is not null)
        {
            if (controller is not null)
            {
                controller.GuideMode = Program.Options.GuideButton;
                controller.Poll(ImGui.GetTime(), Program.Options.DesktopGamepad);
                if (controller.AppControl != appControlOn) SetAppControl(controller.AppControl, true);
            }
            else if (appControlOn) SetAppControl(false, false);
            if (streamSettings)
            {
                // Settings opened from the stream screen: a full-window page over the (hidden) picture; Back returns to the stream.
                draw.AddRectFilled(Vector2.Zero, size, Color(.03f, .045f, .105f, .985f));
                Settings();
            }
            else StreamScreen(size);
        }
        else
        {
            draw.AddRectFilledMultiColor(Vector2.Zero, size, Color(.035f, .025f, .085f), Color(.025f, .075f, .15f), Color(.015f, .025f, .065f), Color(.10f, .018f, .09f));
            draw.AddCircleFilled(At(150, 90), 245 * scale, Color(.08f, .32f, .52f, .16f), 64);
            draw.AddCircleFilled(At(1125, 120), 290 * scale, Color(.25f, .10f, .40f, .14f), 64);
            draw.AddCircleFilled(At(170, 710), 300 * scale, Color(.10f, .34f, .40f, .14f), 64);
            bool onSettings = connecting is null && page == Page.Settings;
            if (!onSettings)
            {
                // Logo and name centred as one group, so the page is symmetrical about its vertical axis.
                float titleWidth = Measure("NX Remote Play", 48, true).X, logoWidth = 40 * logo!.Width / (float)logo.Height;
                float groupLeft = (1280 - (logoWidth + 18 + titleWidth)) / 2;
                draw.AddImage(logo.Texture, At(groupLeft, 38), At(groupLeft + logoWidth, 78));
                Text("NX Remote Play", groupLeft + logoWidth + 18, 58 - Measure("NX Remote Play", 48, true).Y / 2, 48, true);
            }
            if (connecting is not null)
            {
                if (!usbSession) CenterText(status, 0, 305, 1280, 38, true);   // opening the USB Switch is announced by its notice instead
                if (Button("Cancel", 470, 435, 340, 66)) UserDisconnect();
            }
            else if (page == Page.Home)
            {
                ImGui.SetCursorScreenPos(At(280, 210));
                bool pressed = ImGui.InvisibleButton("Connect card", new Vector2(340, 350) * scale);
                ImGui.SetItemDefaultFocus();
                bool selected = ImGui.IsItemHovered() || ImGui.IsItemFocused();
                CardGradient(280, false);
                draw.AddRect(At(280, 210), At(620, 560), selected ? Color(.42f, .76f, 1) : Color(.72f, .84f, 1, .47f), 28 * scale, ImDrawFlags.RoundCornersAll, selected ? 3 * scale : 2 * scale);
                float lineHeight = Measure("Connect To", 74, true).Y;
                float groupTop = 210 + (350 - (69 + 21 + lineHeight * 2 + 4)) / 2;
                draw.AddImage(joycon!.Texture, At(410, groupTop), At(490, groupTop + 69));
                CenterText("Connect To", 280, groupTop + 90, 340, 74, true);
                CenterText("A Switch", 280, groupTop + 94 + lineHeight, 340, 74, true);
                if (pressed) { page = Page.Join; error = ""; }
                // Second card: open the USB Switch by hand (automatic opening may be off, or the user ended the connection).
                ImGui.SetCursorScreenPos(At(660, 210));
                bool usbPressed = ImGui.InvisibleButton("USB card", new Vector2(340, 350) * scale);
                bool usbSelected = ImGui.IsItemHovered() || ImGui.IsItemFocused();
                CardGradient(660, true);
                draw.AddRect(At(660, 210), At(1000, 560), usbSelected ? Color(.42f, .76f, 1) : Color(.72f, .84f, 1, .47f), 28 * scale, ImDrawFlags.RoundCornersAll, usbSelected ? 3 * scale : 2 * scale);
                Icon(draw, "plug", At(830, groupTop + 34), 66 * scale, White);
                CenterText("Connect By", 660, groupTop + 90, 340, 74, true);
                CenterText("USB", 660, groupTop + 94 + lineHeight, 340, 74, true);
                if (usbPressed) OpenUsbByHand();
            }
            else if (page == Page.Join)
            {
                draw.AddRectFilled(At(147, 175), At(1133, 552), Color(.14f, .18f, .29f), 28 * scale);
                CenterText("Connect To A Switch", 147, 190, 986, 59.8f, true);
                CenterText("Enter the details shown on the Switch's Host screen", 147, 262, 986, 28, false, Muted);
                Field("Session ID", ref id, 187, 316, 421, max: 6);
                Field("Password", ref password, 650, 316, 443, false, 6);
                if (Button("Join", 187, 454, 906, 66, 38, true, primary: true)) Join();
            }
            else Settings();
            if (!onSettings)
            {
                if (Button(page == Page.Home ? "Settings" : "Back", 71, 650, 180, 48, 30)) { page = page == Page.Home ? Page.Settings : Page.Home; error = ""; }
                // Mirrors the Settings button: same margin from the right edge, same vertical centre.
                Text(status, 1209 - Measure(status, 24).X, 674 - Measure(status, 24).Y / 2, 24, false, Muted);
                // Errors are centred cards in the pop-up style (one at a time): the USB error on the first page, a join error on the Join page.
                string shown = page == Page.Home ? (usbError.Length > 0 ? usbError : error) : (error.Length > 0 ? error : usbError);
                if (shown.Length > 0) ErrorCard(shown, 640, page == Page.Home ? 646 : 574);
            }
            u = Math.Clamp(scale, .72f, 1.6f);
            DrawToasts(size, 88 * scale);   // pop-ups (USB events, link copied, folder saved) sit below the title group
        }
        EditPopup();
        UpdatePopup();
        modalOpen = ImGui.IsPopupOpen("Edit setting") || ImGui.IsPopupOpen("OnScreenKeyboard") || ImGui.IsPopupOpen(UpdatePopupId);
        if (controller is not null && player is not null && !streamSettings)
        {
            // Touch follows the picture wherever the viewport puts it (rotation included).
            var shown = player.ScreenRect;
            var local = ImGui.GetIO().MousePos - new Vector2(shown.X, shown.Y);
            bool inside = shown.W > 0 && shown.H > 0 && local.X >= 0 && local.Y >= 0 && local.X < shown.W && local.Y < shown.H;
            bool pressed = ImGui.IsMouseDown(ImGuiMouseButton.Left) && !streamMenu && !ImGui.IsAnyItemHovered() && inside;
            float nx = inside ? local.X / shown.W : 0, ny = inside ? local.Y / shown.H : 0;
            (float x, float y) = rotation switch { 1 => (ny, 1 - nx), 2 => (1 - nx, 1 - ny), 3 => (1 - ny, nx), _ => (nx, ny) };
            controller.Touch(pressed, (ushort)Math.Clamp((int)(x * 1280), 0, 1279), (ushort)Math.Clamp((int)(y * 720), 0, 719));
        }
        ImGui.End();
    }
    // Once per start, outside a stream: tells the user a newer release is on GitHub.
    const string UpdatePopupId = "Update available";
    bool updatePopupShown;
    void UpdatePopup()
    {
        if (!updatePopupShown && UpdateCheck.Current == UpdateCheck.State.Available && player is null && connecting is null && !ImGui.IsPopupOpen("Edit setting"))
        {
            updatePopupShown = true;
            ImGui.OpenPopup(UpdatePopupId);
        }
        var size = ImGui.GetIO().DisplaySize;
        ImGui.SetNextWindowPos(Vector2.Zero); ImGui.SetNextWindowSize(size);
        ImGui.PushStyleColor(ImGuiCol.WindowBg, new Vector4(0, 0, 0, .45f));
        ImGui.PushStyleVar(ImGuiStyleVar.WindowBorderSize, 0); ImGui.PushStyleVar(ImGuiStyleVar.WindowPadding, Vector2.Zero);
        bool open = ImGui.BeginPopupModal(UpdatePopupId, ImGuiWindowFlags.NoDecoration | ImGuiWindowFlags.NoMove | ImGuiWindowFlags.NoSavedSettings | ImGuiWindowFlags.NoScrollbar);
        ImGui.PopStyleVar(2); ImGui.PopStyleColor();
        if (!open) return;
        var saved = draw; draw = ImGui.GetWindowDrawList();
        draw.AddRectFilled(At(240, 200), At(1040, 500), Color(.06f, .09f, .17f, .985f), 30 * scale);
        draw.AddRect(At(240, 200), At(1040, 500), Color(.30f, .45f, .72f, .7f), 30 * scale, ImDrawFlags.RoundCornersAll, 2 * scale);
        Text("Update available", 282, 232, 34, true);
        Text("Version " + UpdateCheck.Latest + " is out. You have " + UpdateCheck.Version + ".", 282, 290, 24, false, Muted);
        Text(UpdateCheck.ReleasesPageLabel, 282, 330, 22, false, Muted);
        if (Button("Open releases page", 282, 400, 330, 62, 28, true, primary: true)) { SystemUtil.OpenURL(UpdateCheck.ReleasesPage); ImGui.CloseCurrentPopup(); }
        if (Button("Later", 632, 400, 200, 62, 28) || ImGui.IsKeyPressed(ImGuiKey.Escape)) ImGui.CloseCurrentPopup();
        draw = saved;
        ImGui.EndPopup();
    }
    // ---- Settings page: sidebar + card layout shared by the Windows, Android and Switch (NRO) settings designs ----
    const string VersionLabel = "";
    static readonly string[] TabNames = { "General", "Connection & USB", "Playback & Controls", "Web Sharing", "Advanced & Performance", "Diagnostics & Logging" };
    static readonly string[] TabIcons = { "sliders", "wifi", "pad", "globe", "chip", "doc" };
    static readonly string[] TabSubtitles =
    {
        "Language, privacy and where your files are saved.",
        "How the app finds your Switch and what it receives over USB.",
        "How the picture and sound play, and how you control the app.",
        "Watch and play from a web browser, on your network or the internet.",
        "Decoder, rendering and USB tuning. The defaults suit most people.",
        "Troubleshooting switches for finding problems. Leave off normally.",
    };
    int tab;
    bool tabChanged;
    sealed record Item(string Icon, string Name, string Desc, Func<bool>? Get = null, Action<bool>? Set = null, string Value = "", Action? Activate = null, bool External = false, Action<int>? Step = null);

    static void Icon(ImDrawListPtr d, string kind, Vector2 c, float s, uint col)
    {
        float u = s / 2, t = Math.Max(1.5f, s * .075f), dot = .1f * u + t * .4f;
        Vector2 P(float x, float y) => c + new Vector2(x * u, y * u);
        switch (kind)
        {
            case "wifi":
                var o = P(0, .6f);
                d.AddCircleFilled(o, dot * 1.3f, col);
                foreach (var r in new[] { .5f, .9f, 1.3f }) { d.PathArcTo(o, r * u, -MathF.PI * .75f, -MathF.PI * .25f, 16); d.PathStroke(col, ImDrawFlags.None, t); }
                break;
            case "pad":
                d.AddRect(P(-1.15f, -.65f), P(1.15f, .65f), col, .6f * u, ImDrawFlags.RoundCornersAll, t);
                d.AddLine(P(-.85f, 0), P(-.25f, 0), col, t); d.AddLine(P(-.55f, -.3f), P(-.55f, .3f), col, t);
                d.AddCircleFilled(P(.45f, .15f), dot, col); d.AddCircleFilled(P(.8f, -.15f), dot, col);
                break;
            case "globe":
                d.AddCircle(c, 1.05f * u, col, 32, t);
                d.AddLine(P(-1.05f, 0), P(1.05f, 0), col, t);
                Vector2 prev = default;
                for (int i = 0; i <= 24; i++)
                {
                    float a = i * MathF.PI * 2 / 24; var p = P(MathF.Cos(a) * .45f, MathF.Sin(a) * 1.05f);
                    if (i > 0) d.AddLine(prev, p, col, t);
                    prev = p;
                }
                break;
            case "lock":
                d.AddRect(P(-.8f, -.1f), P(.8f, 1f), col, .25f * u, ImDrawFlags.RoundCornersAll, t);
                d.PathArcTo(P(0, -.1f), .5f * u, MathF.PI, MathF.PI * 2, 16); d.PathStroke(col, ImDrawFlags.None, t);
                d.AddCircleFilled(P(0, .45f), dot, col);
                break;
            case "window":
                d.AddRect(P(-1.05f, -.8f), P(1.05f, .8f), col, .22f * u, ImDrawFlags.RoundCornersAll, t);
                d.AddLine(P(-1.05f, -.35f), P(1.05f, -.35f), col, t);
                break;
            case "plug":
                d.AddRect(P(-.6f, -.15f), P(.6f, .6f), col, .15f * u, ImDrawFlags.RoundCornersAll, t);
                d.AddLine(P(-.3f, -.15f), P(-.3f, -.85f), col, t); d.AddLine(P(.3f, -.15f), P(.3f, -.85f), col, t);
                d.AddLine(P(0, .6f), P(0, 1.05f), col, t);
                break;
            case "doc":
                d.AddRect(P(-.75f, -1f), P(.75f, 1f), col, .2f * u, ImDrawFlags.RoundCornersAll, t);
                d.AddLine(P(-.4f, -.4f), P(.4f, -.4f), col, t); d.AddLine(P(-.4f, 0), P(.4f, 0), col, t); d.AddLine(P(-.4f, .4f), P(.1f, .4f), col, t);
                break;
            case "ext":
                d.AddRect(P(-1f, -.7f), P(.5f, 1f), col, .2f * u, ImDrawFlags.RoundCornersAll, t);
                d.AddLine(P(-.1f, .1f), P(1f, -1f), col, t);
                d.AddLine(P(.2f, -1f), P(1f, -1f), col, t); d.AddLine(P(1f, -1f), P(1f, -.2f), col, t);
                break;
            case "router":
                d.AddRect(P(-1.1f, .05f), P(1.1f, .85f), col, .25f * u, ImDrawFlags.RoundCornersAll, t);
                d.AddLine(P(-.6f, .05f), P(-.9f, -.9f), col, t); d.AddLine(P(.6f, .05f), P(.9f, -.9f), col, t);
                d.AddCircleFilled(P(-.55f, .45f), dot, col); d.AddCircleFilled(P(-.15f, .45f), dot, col);
                break;
            case "sliders":
                for (int i = 0; i < 3; i++)
                {
                    float y = -.7f + i * .7f, k = i == 0 ? -.4f : i == 1 ? .5f : -.1f;
                    d.AddLine(P(-1f, y), P(1f, y), col, t); d.AddCircleFilled(P(k, y), .26f * u + t * .3f, col);
                }
                break;
            case "chip":
                d.AddRect(P(-.65f, -.65f), P(.65f, .65f), col, .15f * u, ImDrawFlags.RoundCornersAll, t);
                foreach (var k in new[] { -.3f, .3f })
                {
                    d.AddLine(P(k, -.65f), P(k, -1f), col, t); d.AddLine(P(k, .65f), P(k, 1f), col, t);
                    d.AddLine(P(-.65f, k), P(-1f, k), col, t); d.AddLine(P(.65f, k), P(1f, k), col, t);
                }
                break;
            default: // "ring"
                d.AddCircle(c, 1.05f * u, col, 32, t); d.AddCircle(c, .45f * u, col, 24, t);
                break;
        }
    }
    void SaveSettings()
    {
        Program.Options.LastRelayServer = server;
        Program.Options.DesktopAutoUsb = autoUsb;
        try { SystemUtil.StoreSettingsString(Program.Options.SerializeToJson()); Owner.ApplyImguiSettings(); }
        catch (Exception ex) { error = "Could not save settings: " + ex.Message; }
    }
    List<Item> SettingsItems(int which)
    {
        var opt = Program.Options;
        var items = new List<Item>();
        Item Toggle(string icon, string name, string desc, Func<bool> get, Action<bool> set) => new(icon, name, desc, get, set);
        Item Edit(string icon, string name, string desc, string? value, Action<string> apply) => new(icon, name, desc, null, null,
            string.IsNullOrEmpty(value) ? "Not set" : value.Length > 18 ? value[..15] + "..." : value,
            () => { editLabel = name; editValue = value ?? ""; applyEdit = apply; openEditPopup = true; });
        Item EnumRow<T>(string icon, string name, string desc, Func<T> get, Action<T> set, Func<T, string>? label = null) where T : struct, Enum => new(icon, name, desc, null, null, label?.Invoke(get()) ?? get().ToString(),
            () => { var values = Enum.GetValues<T>(); set(values[(Array.IndexOf(values, get()) + 1) % values.Length]); },
            Step: direction => { var values = Enum.GetValues<T>(); set(values[(Array.IndexOf(values, get()) + direction + values.Length) % values.Length]); });
        Item Stepper(Item item, Action<int> step) => item with { Step = step };
        void ApplyCustomServer(string value)
        {
            if (!NxRemotePlay.Client.Core.ServerAddress.TryParse(value, out _, out _)) throw new ArgumentException("Enter an IP address or a name, with an optional :port (7921 if none).");
            opt.CustomServer = value.Trim();
            server = NxRemotePlay.Client.Core.ServerAddress.Effective(opt);
        }
        switch (which)
        {
            case 0: // General
                items.Add(Toggle("ring", "Hide console serials from GUI", "Replaces each Switch's serial number with a placeholder wherever the app would show it, such as in device lists and stream information. Turn it on before sharing screenshots or recordings so your console's serial number stays private.", () => opt.HideSerials, value => opt.HideSerials = value));
                items.Add(Toggle("ring", "Streamer mode", "Keeps private details off the screen while you stream or record. The session ID, addresses and links on the stream screen turn into dots, and the ID and password fields are masked as you type. You can also switch it on and off during a stream with the Streamer button in the side panel.", () => opt.StreamerMode, value => opt.StreamerMode = value));
                items.Add(new("doc", "Screenshots folder", "The folder where screenshots are saved, as PNG pictures named NxRemotePlay_date_time.png. Click to choose a different folder. It is not used while Copy screenshots to clipboard is on.", Value: ShortPath(opt.ScreenshotsPath),
                    Activate: () => PickFolder("Choose the folder for screenshots", opt.ScreenshotsPath, path => opt.ScreenshotsPath = path)));
                items.Add(new("doc", "Recordings folder", "The folder where recordings are saved, as MP4 videos named NxRemotePlay_date_time.mp4. Click to choose a different folder. Pick a drive with plenty of free space, because video files grow quickly.", Value: ShortPath(opt.RecordingsPath),
                    Activate: () => PickFolder("Choose the folder for video recordings", opt.RecordingsPath, path => opt.RecordingsPath = path)));
                items.Add(Toggle("doc", "Copy screenshots to clipboard", "When on, taking a screenshot copies the picture to the Windows clipboard so you can paste it straight into a chat or an editor, instead of saving a file. Hold Shift while taking a screenshot to do the opposite for that one shot.", () => opt.Windows_ScreenToClip, value => opt.Windows_ScreenToClip = value));
                items.Add(new("ring", "Check for updates", "Asks GitHub whether a newer NX-Remote-Play release is out, the same check the app makes each time it starts. Click to check again. Version " + UpdateCheck.Version + " is running. The check only reads the public release list; github.com sees this computer's network address.", Value: UpdateCheck.StatusText, Activate: UpdateCheck.Start));
                items.Add(new("ext", "Releases page", "Opens the NX-Remote-Play releases page on GitHub in your web browser, where every version can be downloaded.", Value: UpdateCheck.ReleasesPageLabel, Activate: () => SystemUtil.OpenURL(UpdateCheck.ReleasesPage), External: true));
                break;
            case 1: // Connection & USB
                items.Add(Toggle("ring", "Use default server", "On: the app uses the public NX-Remote-Play server. It introduces this computer to a console you join with a Session ID and password, and it helps web browsers connect to this computer. Turn it off to use your own server instead, which you set in the next row.", () => NxRemotePlay.Client.Core.ServerAddress.UsesDefault(opt), value =>
                {
                    opt.UseDefaultServer = value;
                    server = NxRemotePlay.Client.Core.ServerAddress.Effective(opt);
                    if (!value && string.IsNullOrWhiteSpace(opt.CustomServer)) { editLabel = "Server (IP or name, optional :port)"; editValue = ""; applyEdit = ApplyCustomServer; openEditPopup = true; }
                }));
                items.Add(Edit("globe", "Server", "The address of your own NX-Remote-Play server: an IP address or a name, with an optional :port (7921 if you leave it out). It is only used while Use default server is off. Shows Not set until you enter one.", opt.CustomServer, ApplyCustomServer));
                items.Add(Toggle("plug", "Automatically open USB Switch", "When on, the app looks for a Switch connected by USB and opens it for you as soon as the app starts or the cable is plugged in, with no clicking needed. Turn it off if you would rather choose when to connect.", () => autoUsb, value =>
                {
                    autoUsb = value;
                    if (value) { usbUnavailable = false; driverTried = false; usbError = ""; nextScan = 0; }
                }));
                items.Add(EnumRow("plug", "USB streaming mode", "What the Switch sends over USB. Both means picture and sound, Video means picture only, Audio means sound only. Click to change. Both is right for nearly everyone. Web browsers that watch through this app receive only what is sent. Applies the next time you connect.", () => opt.Streaming.Kind, value => opt.Streaming.Kind = value));
                items.Add(Toggle("window", "Turn off console backlight during streaming", "Switches the console's own screen off while this app is streaming it, then back on when you disconnect. Handy when you only watch on the PC. It is experimental: if the screen stays dark after something goes wrong, lock and unlock the console (press its power button) to bring it back. Applies the next time you connect.", () => opt.Streaming.TurnOffConsoleScreen, value => opt.Streaming.TurnOffConsoleScreen = value));
                break;
            case 2: // Playback & Controls
                items.Add(EnumRow("window", "Scale mode (restart)", "How the picture is stretched to fit the window. Linear (default) is smooth. Nearest keeps hard pixel edges and takes the least effort. Best asks Windows for the highest quality it can give. Click to change. Takes effect after you restart the app.", () => opt.RendererScale, value => opt.RendererScale = value));
                items.Add(EnumRow("window", "Picture upscaling (restart)", "Makes the picture look sharper when the window is bigger than the Switch's 1280x720. Off (default) is the normal smooth stretch. Fsr uses AMD FidelityFX Super Resolution 1 on your graphics card: it rebuilds edges instead of just blurring them bigger, then sharpens them (best quality). Cubic is a lighter filter for weak graphics cards. Nothing changes on the Switch. It needs the OpenGL graphics mode, so the app switches to it when this is on. If the picture or the menus look wrong, set it back to Off. Takes effect after you restart the app.", () => opt.Upscale, value => opt.Upscale = value));
                items.Add(Stepper(Edit("window", "Upscaling sharpness", "How strong the final sharpening is, from 0 to 100. 75 is a good start (AMD's own default); higher looks crisper but can show outlines around bright text, 0 is the softest. Used by Fsr and Cubic. Click to type a number. Takes effect after you restart the app.", opt.UpscaleStrength.ToString(), value =>
                {
                    if (!int.TryParse(value.Trim(), out int strength) || strength is < 0 or > 100) throw new ArgumentException("Enter a number from 0 to 100.");
                    opt.UpscaleStrength = strength;
                }), direction => opt.UpscaleStrength = Math.Clamp(opt.UpscaleStrength + direction * 5, 0, 100)));
                items.Add(EnumRow("ring", "Audio player mode", "How the app plays the Switch's sound. Auto (default) chooses for you and, on Windows, behaves like Default, which keeps sound lined up with the picture. Compatible uses a simpler player: try it if you hear crackling or gaps, or get no sound. Applies the next time you connect.", () => opt.AudioPlayerMode, value => opt.AudioPlayerMode = value));
                items.Add(Stepper(Edit("ring", "Default audio volume", "The volume, from 0 to 100, that every new stream starts at. You can still change it during a stream with the Up and Down arrow keys or the volume slider in the menu. Click to type a number.", opt.DefaultVolume.ToString(), value =>
                {
                    if (!int.TryParse(value.Trim(), out int volume) || volume is < 0 or > 100) throw new ArgumentException("Enter a number from 0 to 100.");
                    opt.DefaultVolume = volume;
                }), direction => opt.DefaultVolume = Math.Clamp(opt.DefaultVolume + direction * 5, 0, 100)));
                items.Add(Toggle("chip", "Show FPS over the picture", "Shows how many video frames per second the app is really receiving, in a corner of the picture, even in full screen or with the interface hidden. A steady number matching the Switch's frame rate (30 or 60) means a smooth stream. Low or jumping numbers point to a network, USB or console problem.", () => opt.ShowFpsOverlay, value => opt.ShowFpsOverlay = value));
                {
                    items.Add(Toggle("video", "Virtual camera (OBS, Discord, Zoom)", "Shows the game picture as a camera named NX Remote Play Camera that other programs can pick, such as OBS, Discord, Zoom or Chrome. It is only the picture: not this window, its buttons, or the dots of streamer mode. Install the camera once with the next row (that also turns this on). Nothing is converted unless a program is using the camera, and the camera shows a dark picture while no stream is running. The app writes what the camera is doing to desktop-video.log.", () => opt.VirtualCamera, value => { opt.VirtualCamera = value; NxRemotePlay.Client.Targets.VirtualCameraFeed.Apply(value); }));
                    items.Add(new("video", NxRemotePlay.Client.Targets.VirtualCameraFeed.IsInstalled ? "Remove the virtual camera" : "Install the virtual camera", "Adds or removes the camera for your Windows account only, so it needs no administrator rights. The app copies a small file to your AppData folder and registers it; programs that are already open need a restart to see the camera. Browsers and camera programs keep the file open while they run, so after Remove the old file may stay until they are closed; installing again always works. Windows privacy settings must allow desktop apps to use the camera. Only 64-bit programs can use it.", Value: NxRemotePlay.Client.Targets.VirtualCameraFeed.Status, Activate: NxRemotePlay.Client.Targets.VirtualCameraFeed.InstallOrRemoveInBackground));
                    items.Add(new("video", NxRemotePlay.Client.Targets.VirtualCameraFeed.IsInstalledAllUsers ? "Remove the camera for programs run as administrator" : "Install the camera for programs run as administrator", "Some programs are started as administrator, and OBS is often set that way (Windows Run as administrator). Windows hides a camera that is installed for your account only from such programs, so they do not list NX Remote Play Camera. This adds the camera for all users instead and asks Windows for permission (a prompt appears). Use it if OBS or another program run as administrator does not show the camera; restart that program afterwards.", Value: NxRemotePlay.Client.Targets.VirtualCameraFeed.AllUsersStatus, Activate: NxRemotePlay.Client.Targets.VirtualCameraFeed.InstallAllUsersOrRemoveInBackground));
                }
                items.Add(Toggle("ring", "Enable hotkeys in the player view", "Keyboard shortcuts while streaming: S takes a screenshot, R starts or stops recording, T rotates the picture, F toggles full screen, and Up and Down change the volume. H, which hides or shows the text on the picture, always works. Turn this off if these keys get in your way.", () => opt.PlayerHotkeys, value => opt.PlayerHotkeys = value));
                items.Add(Toggle("pad", "Forward desktop gamepad during remote play", "Sends a controller connected to this computer to the Switch, as if it were plugged into the console, so you can play remotely with it. Turn it off to stop this computer's controller from reaching the Switch.", () => opt.DesktopGamepad, value => opt.DesktopGamepad = value));
                items.Add(Toggle("pad", "Control this app with a gamepad", "Lets a controller connected to this computer move around the app's menus and press their buttons, such as these settings, instead of using the mouse. While you stream with forwarding turned on, the controller goes to the Switch instead of the menus.", () => opt.ControllerInput, value => opt.ControllerInput = value));
                items.Add(EnumRow("pad", "Guide button", "The big round button in the middle of your controller (Xbox, PlayStation or Home button). Home: it presses HOME on the Switch. App toggle: each press switches your controller between playing on the Switch and moving around this app, and a notice tells you which one is active. Tap / hold: a quick tap presses HOME, and holding it for half a second does the switch. While the controller is moving the app, nothing it does reaches the Switch, and with the interface hidden (full screen) a side panel with every control comes up. Needs Forward desktop gamepad turned on. If nothing happens, Windows may be keeping the button for the Xbox Game Bar: turn that off in Windows settings.",
                    () => opt.GuideButton, value => opt.GuideButton = value,
                    mode => mode switch { GuideButtonMode.Home => "Home", GuideButtonMode.AppControl => "App toggle", _ => "Tap / hold" }));
                break;
            case 3: // Web Sharing
                items.Add(Toggle("window", "Browser sharing over USB (next connection)", "While a Switch is connected by USB, the app also hosts a web page so a phone, tablet or another computer can watch and play in an ordinary web browser, with nothing to install. The Open and Copy buttons for its link are in the stream screen's side panel. Applies the next time you connect.", () => opt.DesktopBrowserSharing, value => opt.DesktopBrowserSharing = value));
                items.Add(Toggle("globe", "Public browser sharing (next connection)", "Makes the link work from outside your home network as well. The app finds your public IP address and builds a public link, and your router must let the web and video ports through (the next setting asks it to). Anyone who has the link can join without a password until the USB session ends, so share it carefully. Applies the next time you connect.", () => opt.DesktopBrowserPublicSharing, value => opt.DesktopBrowserPublicSharing = value));
                items.Add(Toggle("router", "Request router ports for browser sharing", "Asks your router to open the web (HTTPS) port and the video (UDP) port for you, using NAT-PMP, so public sharing works without setting up port forwarding by hand. A router that does not support NAT-PMP ignores the request, and then you must forward those two ports yourself in the router's settings. Port 80 is also used briefly when an automatic certificate is requested.", () => opt.DesktopBrowserRouterMapping, value => opt.DesktopBrowserRouterMapping = value));
                items.Add(Stepper(Edit("window", "Browser WebRTC UDP port", "The UDP port that carries live video and sound to web browsers (WebRTC). Default 8444. It must be an even number from 2 to 65534. It is used with public sharing. If you forward ports on your router yourself, forward this one (UDP) to this computer. Change it only if another program already uses it. Applies the next time you connect.", opt.DesktopBrowserMediaPort.ToString(), value =>
                {
                    if (!int.TryParse(value, out int port) || port is < 2 or > 65534 || port % 2 != 0) throw new ArgumentException("Enter an even UDP port from 2 to 65534.");
                    opt.DesktopBrowserMediaPort = port;
                }), direction => opt.DesktopBrowserMediaPort = Math.Clamp(opt.DesktopBrowserMediaPort + direction * 2, 2, 65534) / 2 * 2));
                items.Add(Stepper(Edit("lock", "Browser HTTPS port (next connection)", "The port of the sharing web page, which is the number after the address in the link. Default 8443. Any number from 1 to 65535. Change it if another program already uses 8443, or to match a port you set up on your router. Applies the next time you connect.", opt.DesktopBrowserPort.ToString(), value =>
                {
                    if (!int.TryParse(value, out int port) || port is < 1 or > 65535) throw new ArgumentException("Enter a port from 1 to 65535.");
                    opt.DesktopBrowserPort = port;
                }), direction => opt.DesktopBrowserPort = Math.Clamp(opt.DesktopBrowserPort + direction, 1, 65535)));
                items.Add(Toggle("globe", "Automatic public-IP HTTPS certificate", "Without a trusted certificate, browsers warn that the page is not secure, because the app makes its own. When on, the app gets a free trusted certificate for your public IP address from Let's Encrypt and renews it by itself. It needs Public browser sharing, the router ports request and the agreement below accepted, and port 80 must be reachable for a moment while the certificate is issued.", () => opt.DesktopBrowserAutomaticCertificate, value => opt.DesktopBrowserAutomaticCertificate = value));
                items.Add(Edit("doc", "Trusted browser certificate PFX", "Optional. The full path to a certificate file (.pfx) of your own, so browsers trust the page without a warning. Use it instead of the automatic certificate if you already have one for your address. Leave it empty to use the built-in self-signed certificate. Applies the next time you connect.", opt.DesktopBrowserCertificatePath, value => opt.DesktopBrowserCertificatePath = value.Trim()));
                items.Add(new("ext", "Let's Encrypt subscriber agreement", "Opens the Let's Encrypt subscriber agreement in your web browser so you can read the terms of the free certificate service. It only opens the page and changes nothing.", Activate: () => SystemUtil.OpenURL(Rendezvous.AcmeIpCertificate.AgreementUrl), External: true));
                items.Add(Toggle("ring", "I accept the certificate subscriber agreement", "Turn on to confirm that you have read and agree to the Let's Encrypt agreement. The app will not request an automatic certificate without it, so the automatic certificate setting does nothing until this is on.", () => opt.DesktopBrowserCertificateAgreementAccepted, value => opt.DesktopBrowserCertificateAgreementAccepted = value));
                break;
            case 4: // Advanced & Performance
                items.Add(Edit("chip", "Hardware decoder", "Which FFmpeg decoder turns the Switch's H.264 video back into a picture. Automatic (recommended) uses the normal decoder, which is plenty fast on a regular PC. Only enter a name if you have an FFmpeg build that includes a hardware decoder, for example on a small PC such as a Raspberry Pi. If it is not available the player shows an error, so type Automatic to go back. Applies the next time you connect.", opt.DecoderName ?? "Automatic", value => opt.DecoderName = value == "Automatic" || value.Length == 0 ? null : value));
                items.Add(Toggle("chip", "Force software rendering (restart)", "Draws the picture with the processor instead of the graphics card. Try it if the window is black, flickers or crashes, which can happen with some graphics drivers or remote-desktop sessions. It uses more CPU and can be slower. Takes effect after you restart the app.", () => opt.ForceSoftwareRenderer, value => opt.ForceSoftwareRenderer = value));
                items.Add(Toggle("chip", "Use CPU SIMD instructions", "Lets the processor handle audio in bigger batches with its vector (SIMD) instructions. It is only used when saving recordings, to prepare the sound for the MP4 file, and has no effect on watching a stream. Leave it on unless a recording has audio problems.", () => opt.UseSIMDAcceleration, value => opt.UseSIMDAcceleration = value));
                items.Add(Toggle("chip", "Uncap streaming framerate (restart)", "The stream screen normally redraws at most 60 times a second. When on, that limit is removed and the window redraws in step with your monitor's refresh rate, which can cut a little delay on 120 or 144 Hz screens but uses more power. Takes effect after you restart the app.", () => opt.UncapStreaming, value => opt.UncapStreaming = value));
                items.Add(Toggle("chip", "Uncap GUI framerate (restart)", "Removes the limit on how often the app's own screens redraw, so they follow your monitor's refresh rate. It can feel smoother on high refresh rate screens but raises CPU and graphics use. Rarely needed. Takes effect after you restart the app.", () => opt.UncapGUI, value => opt.UncapGUI = value));
                items.Add(Stepper(Edit("plug", "USB audio batching", "How many sound chunks the Switch bundles into each USB packet: every packet carries this number plus one. Range 0 to 5, default 3. Higher means fewer, larger packets, which helps if USB hiccups, at the cost of a little more sound delay. Lower reacts faster but may crackle on a weak connection. Applies the next time you connect.", opt.Streaming.AudioBatching.ToString(), value =>
                {
                    if (!int.TryParse(value.Trim(), out int batching) || batching < 0 || batching > StreamInfo.MaxAudioBatching) throw new ArgumentException("Enter a number from 0 to " + StreamInfo.MaxAudioBatching + ".");
                    opt.Streaming.AudioBatching = batching;
                }), direction => opt.Streaming.AudioBatching = Math.Clamp(opt.Streaming.AudioBatching + direction, 0, StreamInfo.MaxAudioBatching)));
                items.Add(Toggle("plug", "Cache USB video packets locally", "Saves USB bandwidth. When the Switch would send a video packet identical to a recent one, it sends a tiny reference instead and this app reuses its saved copy. It costs a little memory and processing. Turn it off to test whether it causes picture glitches. Applies the next time you connect.", () => opt.Streaming.UseNALReplay, value => opt.Streaming.UseNALReplay = value));
                items.Add(Toggle("plug", "Apply USB packet cache only to keyframes", "Limits the packet cache above to keyframes, the full pictures in H.264 video that the frames between them build on. This is the default and keeps the extra work small. Turn it off to cache every video packet. It does nothing while the cache is off. Applies the next time you connect.", () => opt.Streaming.UseNALReplayOnlyOnKeyframes, value => opt.Streaming.UseNALReplayOnlyOnKeyframes = value));
                items.Add(Toggle("doc", "Disable Audio/Video synchronization", "Normally the app skips sound that arrives more than about 0.09 seconds late compared with the picture, so the two stay in step. With this on, nothing is skipped: sound is kept even when late, so it can lag behind the picture. Mostly useful for testing. Applies the next time you connect.", () => opt.Debug.NoSync, value => opt.Debug.NoSync = value));
                break;
            default: // Diagnostics & Logging
                items.Add(new("doc", "Export logs", "Saves one zip with a support report (app version, system, settings), the app's own logs and the newest connection traces. The logs may contain addresses, identifiers or other sensitive details: review the contents before sharing. The zip is saved in the Diagnostics folder, which opens afterwards. The Switch has its own logs: use Export logs in the Switch app's Settings.", Activate: () =>
                {
                    try { var path = SupportReport.SaveBundle(); Notify("Logs exported", Path.GetFileName(path), Good, 8); SystemUtil.OpenURL(Path.GetDirectoryName(path)!); }
                    catch (Exception ex) { Program.LogError("export logs", ex); Notify("Could not export the logs", ex.Message, Bad, 8); }
                }, External: true));
                items.Add(Toggle("doc", "Show framerate/GUI debug info", "Opens a debug window over the app with the interface's own render frame rate and Dear ImGui and SDL statistics. It measures how fast this app draws, not the stream's frame rate (use Show FPS over the picture for that). Useful for tracking down stutter in the interface.", () => opt.Debug.GuiDebug, value => opt.Debug.GuiDebug = value));
                items.Add(Toggle("doc", "Enable verbose logging", "Writes detailed diagnostics to the console the app was started from, and shows packet-level stream errors in the status line instead of only in the console. On a USB connection it also asks the Switch for its memory-pool report during the handshake and logs it. For troubleshooting only; the extra output can slow the app slightly.", () => opt.Debug.Log, value => opt.Debug.Log = value));
                items.Add(Toggle("doc", "Debug log of SDL events", "Prints every SDL event the app receives (keyboard, mouse, controller and window events) to the console as it happens. Use it to see whether a controller, key or window action reaches the app at all.", () => opt.Debug.SDLEvents, value => opt.Debug.SDLEvents = value));
                items.Add(Toggle("doc", "Analyze keyframe NALs", "Inspects the H.264 video as it arrives and prints a line to the console each time a keyframe (an IDR frame, NAL type 5) comes in, with the time since the previous keyframe in milliseconds. Shows how often the Switch sends full pictures. Adds a little processing to every video packet.", () => opt.Debug.Keyframe, value => opt.Debug.Keyframe = value));
                items.Add(Toggle("doc", "Analyze every NAL packet", "Prints a line to the console for every video packet: the milliseconds since the previous packet, then the type number (in hex) of each H.264 NAL unit inside it. For example 1 is a normal frame slice, 5 a keyframe slice, 7 and 8 the stream's settings. Very verbose; adds processing to every video packet.", () => opt.Debug.Nal, value => opt.Debug.Nal = value));
                break;
        }
        return items;
    }
    // Sidebar label: one line shrunk to fit; the longest names wrap onto two smaller lines.
    void TabLabel(string name, float y, uint color)
    {
        const float available = 186;
        float size = 26, width = Measure(name, size, true).X;
        if (width > available) { size = Math.Max(21, size * available / width); width = Measure(name, size, true).X; }
        int cut = -1;
        if (width > available)
        {
            for (int k = 0; k < name.Length; k++) if (name[k] == ' ' && (cut < 0 || Math.Abs(k - name.Length / 2) < Math.Abs(cut - name.Length / 2))) cut = k;
            if (cut > 0) size = 21;
        }
        if (cut <= 0) { Text(name, 110, y + 29 - Measure(name, size, true).Y / 2, size, true, color); return; }
        float line = Measure("Ag", size, true).Y * .82f;
        Text(name[..cut], 110, y + 29 - line, size, true, color);
        Text(name[(cut + 1)..], 110, y + 29, size, true, color);
    }
    // Greedy word wrap in design units.
    List<string> Wrap(string text, float size, float width)
    {
        var lines = new List<string>(); string line = "";
        foreach (var word in text.Split(' '))
        {
            string next = line.Length == 0 ? word : line + " " + word;
            if (line.Length > 0 && Measure(next, size).X > width) { lines.Add(line); line = word; } else line = next;
        }
        if (line.Length > 0) lines.Add(line);
        return lines;
    }
    void Settings()
    {
        var iconColor = Color(.88f, .92f, 1f);
        if (settingsLastFrame != drawFrame - 1) { padZone = PadZone.Tabs; padItem = 0; }
        settingsLastFrame = drawFrame;
        // rings for the controller's own position are only drawn while a controller is being used (the keyboard/mouse have their own)
        bool padRing = WantsPadActions && Owner.Pads is { } pn && pn.RecentlyUsed(ImGui.GetTime());
        draw.AddRect(At(20, 20), At(1260, 700), Color(.30f, .40f, .62f, .28f), 30 * scale, ImDrawFlags.RoundCornersAll, 1.5f * scale);
        draw.AddImage(logo!.Texture, At(56, 44), At(100, 90));
        Text("NX Remote Play", 116, 67 - Measure("NX Remote Play", 36, true).Y / 2, 36, true);
        Text(VersionLabel, 1224 - Measure(VersionLabel, 22).X, 56, 22, false, Muted);

        // Sidebar
        for (int i = 0; i < TabNames.Length; i++)
        {
            float y = 140 + i * 68;
            ImGui.SetCursorScreenPos(At(44, y));
            bool pressed = ImGui.InvisibleButton("tab" + i, new Vector2(260, 58) * scale);
            bool selected = i == tab;
            if (selected) ImGui.SetItemDefaultFocus();
            if (selected) draw.AddRectFilled(At(44, y), At(304, y + 58), Color(.12f, .39f, .91f), 16 * scale);
            else if (ImGui.IsItemHovered()) draw.AddRectFilled(At(44, y), At(304, y + 58), Color(.14f, .19f, .32f), 16 * scale);
            if ((ImGui.IsItemFocused() && !WantsPadActions) || (padRing && padZone == PadZone.Tabs && selected)) draw.AddRect(At(44, y), At(304, y + 58), Color(.42f, .76f, 1), 16 * scale, ImDrawFlags.RoundCornersAll, 2 * scale);
            Icon(draw, TabIcons[i], At(78, y + 29), 30 * scale, selected ? White : iconColor);
            TabLabel(TabNames[i], y, selected ? White : Muted);
            if (pressed && tab != i) { tab = i; tabChanged = true; }
        }
        draw.AddLine(At(326, 130), At(326, 620), Color(.30f, .40f, .62f, .35f), 2 * scale);

        // Content
        Text(TabNames[tab], 356, 96, 46, true);
        Text(TabSubtitles[tab], 356, 152, 22, false, Muted);
        var items = SettingsItems(tab);
        lastItems = items;
        if (items.Count > 0) padItem = Math.Clamp(padItem, 0, items.Count - 1);
        const float cardWidth = 868, cardTop = 194, cardMax = 426, descSize = 18, descLine = 23, minRow = 74, layoutWidth = cardWidth - 16;
        // Each row is as tall as its wrapped description needs, so the full explanation is readable; long tabs scroll.
        var rows = new (float ControlLeft, float Height, List<string> Lines)[items.Count];
        float total = 0;
        for (int i = 0; i < items.Count; i++)
        {
            var item = items[i];
            float controlLeft = item.Get is not null ? layoutWidth - 20 - 66 - 16
                : item.External ? layoutWidth - 24 - 40
                : item.Step is not null ? layoutWidth - 24 - 28 - Measure(item.Value, 24, true).X - 56
                : layoutWidth - 24 - 24 - Measure(item.Value, 24, true).X - 16;
            var lines = Wrap(item.Desc, descSize, controlLeft - 84);
            rows[i] = (controlLeft, Math.Max(minRow, 28 + 6 + lines.Count * descLine + 26), lines);
            total += rows[i].Height;
        }
        float cardHeight = Math.Min(total, cardMax);
        lastHeights = rows.Select(r => r.Height).ToArray();
        draw.AddRectFilled(At(356, cardTop), At(356 + cardWidth, cardTop + cardHeight), Color(.07f, .105f, .19f, .92f), 18 * scale);
        draw.AddRect(At(356, cardTop), At(356 + cardWidth, cardTop + cardHeight), Color(.30f, .40f, .62f, .45f), 18 * scale, ImDrawFlags.RoundCornersAll, 1.5f * scale);
        ImGui.SetCursorScreenPos(At(356, cardTop));
        ImGui.PushStyleColor(ImGuiCol.ChildBg, new Vector4(0, 0, 0, 0));
        ImGui.PushStyleVar(ImGuiStyleVar.WindowPadding, Vector2.Zero);
        ImGui.PushStyleVar(ImGuiStyleVar.ItemSpacing, Vector2.Zero);
        ImGui.BeginChild("SettingsList", new Vector2(cardWidth, cardHeight) * scale, false, total > cardMax ? ImGuiWindowFlags.None : ImGuiWindowFlags.NoScrollbar);
        if (tabChanged) { ImGui.SetScrollY(0); tabChanged = false; padScroll = false; }
        if (padScroll && padZone == PadZone.Items)
        {
            // keep the row the controller is on inside the card
            padScroll = false;
            float rowTop = 0; for (int k = 0; k < padItem && k < lastHeights.Length; k++) rowTop += lastHeights[k];
            float rowBottom = rowTop + (padItem < lastHeights.Length ? lastHeights[padItem] : 0), scrolled = ImGui.GetScrollY() / scale;
            if (rowTop < scrolled) ImGui.SetScrollY(rowTop * scale);
            else if (rowBottom > scrolled + cardHeight) ImGui.SetScrollY((rowBottom - cardHeight) * scale);
        }
        var list = ImGui.GetWindowDrawList();
        float width = ImGui.GetContentRegionAvail().X;
        float vw = width / scale;
        for (int i = 0; i < items.Count; i++)
        {
            var item = items[i];
            float rowHeight = rows[i].Height;
            bool padRow = padRing && padZone == PadZone.Items && i == padItem;
            var p = ImGui.GetCursorScreenPos();
            bool pressed = ImGui.InvisibleButton(item.Name, new Vector2(width, rowHeight * scale));
            bool hotRow = ImGui.IsItemHovered();
            if (hotRow || padRow) list.AddRectFilled(p + new Vector2(5, 4) * scale, p + new Vector2(vw - 5, rowHeight - 4) * scale, Color(.12f, .19f, .34f, .9f), 14 * scale);
            if ((ImGui.IsItemFocused() && !WantsPadActions) || padRow) list.AddRect(p + new Vector2(5, 4) * scale, p + new Vector2(vw - 5, rowHeight - 4) * scale, Color(.42f, .76f, 1), 14 * scale, ImDrawFlags.RoundCornersAll, 2 * scale);
            if (i < items.Count - 1) list.AddLine(p + new Vector2(70, rowHeight - .5f) * scale, p + new Vector2(vw - 20, rowHeight - .5f) * scale, Color(.30f, .40f, .62f, .30f), scale);
            Icon(list, item.Icon, p + new Vector2(40, rowHeight / 2) * scale, 30 * scale, iconColor);

            // right-hand control
            if (item.Get is not null)
            {
                bool on = item.Get();
                var t0 = p + new Vector2(vw - 20 - 66, rowHeight / 2 - 18) * scale;
                list.AddRectFilled(t0, t0 + new Vector2(66, 36) * scale, on ? Color(.07f, .56f, .98f) : Color(.50f, .54f, .62f), 18 * scale);
                list.AddCircleFilled(t0 + new Vector2(on ? 66 - 18 : 18, 18) * scale, 14 * scale, White, 24);
            }
            else if (item.Step is not null)
            {
                // arrows on both sides: back and forward; the value in the middle (click: next, or type it in for numbers)
                float right = vw - 24;
                var valueSize = Measure(item.Value, 24, true);
                float valueLeft = right - 28 - valueSize.X;
                var mouse = (ImGui.GetMousePos() - p) / scale;
                void Chevron(float cx, int direction)
                {
                    bool over = hotRow && mouse.Y > 0 && Math.Abs(mouse.X - cx) < 20;
                    var cc = p + new Vector2(cx, rowHeight / 2) * scale;
                    if (over) list.AddCircleFilled(cc, 19 * scale, Color(.20f, .30f, .52f, .95f), 24);
                    list.AddLine(cc + new Vector2(-4 * direction, -9) * scale, cc + new Vector2(4 * direction, 0) * scale, over ? White : iconColor, 2.4f * scale);
                    list.AddLine(cc + new Vector2(4 * direction, 0) * scale, cc + new Vector2(-4 * direction, 9) * scale, over ? White : iconColor, 2.4f * scale);
                }
                Chevron(right - 6, 1);
                Chevron(valueLeft - 22, -1);
                list.AddText(Owner.DesktopBold, 24 * scale, p + new Vector2(valueLeft, rowHeight / 2 - valueSize.Y / 2) * scale, White, item.Value);
            }
            else
            {
                float right = vw - 24;
                if (item.External) Icon(list, "ext", p + new Vector2(right - 12, rowHeight / 2) * scale, 26 * scale, iconColor);
                else
                {
                    var c = p + new Vector2(right - 6, rowHeight / 2) * scale;
                    list.AddLine(c + new Vector2(-4, -9) * scale, c + new Vector2(4, 0) * scale, iconColor, 2.4f * scale);
                    list.AddLine(c + new Vector2(4, 0) * scale, c + new Vector2(-4, 9) * scale, iconColor, 2.4f * scale);
                    var valueSize = Measure(item.Value, 24, true);
                    list.AddText(Owner.DesktopBold, 24 * scale, p + new Vector2(right - 24 - valueSize.X, rowHeight / 2 - valueSize.Y / 2) * scale, White, item.Value);
                }
            }
            // name (shrunk to fit) and the wrapped description, centred as a block in the row
            float textWidth = rows[i].ControlLeft - 84;
            float nameSize = Math.Min(24, 24 * textWidth / Math.Max(1, Measure(item.Name, 24, true).X));
            float top = (rowHeight - (28 + 6 + rows[i].Lines.Count * descLine)) / 2;
            list.AddText(Owner.DesktopBold, nameSize * scale, p + new Vector2(84, top) * scale, White, item.Name);
            for (int l = 0; l < rows[i].Lines.Count; l++)
                list.AddText(Owner.DesktopFont, descSize * scale, p + new Vector2(84, top + 34 + l * descLine) * scale, Color(.62f, .69f, .84f), rows[i].Lines[l]);
            if (pressed)
            {
                if (item.Get is not null) item.Set!(!item.Get());
                else if (item.Step is not null)
                {
                    float x = (ImGui.GetMousePos().X - p.X) / scale, right = vw - 24, valueLeft = right - 28 - Measure(item.Value, 24, true).X;
                    if (x >= right - 34) item.Step(1);
                    else if (x >= valueLeft - 44 && x <= valueLeft) item.Step(-1);
                    else item.Activate?.Invoke();
                }
                else item.Activate?.Invoke();
                SaveSettings();
            }
        }
        ImGui.EndChild();
        ImGui.PopStyleVar(2);
        ImGui.PopStyleColor();
        if (openEditPopup) { openEditPopup = false; ImGui.OpenPopup("Edit setting"); }

        // Footer: Back (left), status (right)
        ImGui.SetCursorScreenPos(At(48, 634));
        bool back = ImGui.InvisibleButton("settings back", new Vector2(150, 48) * scale);
        bool backHot = ImGui.IsItemHovered() || ImGui.IsItemFocused();
        draw.AddRectFilled(At(48, 634), At(198, 682), backHot ? Color(.17f, .23f, .38f) : Color(.12f, .17f, .29f), 24 * scale);
        u = Math.Clamp(scale, .72f, 1.6f);
        PadGlyph(PadButton.East, At(78, 658), 28 * scale);
        Text("Back", 102, 658 - Measure("Back", 24, true).Y / 2, 24, true);
        if (back) { page = Page.Home; streamSettings = false; error = ""; }
        var statusSize = Measure(status, 22);
        Text(status, 1224 - statusSize.X, 658 - statusSize.Y / 2, 22, false, Color(.88f, .92f, 1f));
        draw.AddCircleFilled(At(1224 - statusSize.X - 18, 658), 6 * scale, Color(.20f, .85f, .40f), 16);
        if (error.Length > 0 || usbError.Length > 0) ErrorCard(error.Length > 0 ? error : usbError, 640, 628);
    }
    // A centred notice card on the connect / settings pages (same look as the pop-ups); coordinates in the 1280x720 design grid.
    void ErrorCard(string text, float designCx, float designTop)
    {
        u = Math.Clamp(scale, .72f, 1.6f);
        var (title, detail) = SplitNotice(text);
        var (w, h) = NoticeSize(title, detail, 700 * scale, 21, 15, 300);
        var p = At(designCx, designTop);
        DrawNotice(p.X - w / 2, p.Y, w, h, title, detail, Warn, 1, 21, 15);
    }
    public override void RawDraw()
    {
        Program.SdlCtx.ClearScreen();
        player?.DrawAsync(false);
    }
    public override void OnAppBackgrounded() { if (!usbSession && (player is not null || connecting is not null)) Disconnect(); }
    public override void OnKeyPressed(SDL2.SDL.SDL_Keysym key)
    {
        // H works even when the other player hotkeys are disabled: hide/show all text over the stream.
        if (player is not null && key.sym == SDL2.SDL.SDL_Keycode.SDLK_h) { hideText = !hideText; return; }
        if (player is null || !Program.Options.PlayerHotkeys) return;
        HandlePlayerKey(key.sym);
    }
    void HandlePlayerKey(SDL2.SDL.SDL_Keycode key)
    {
        if (key == SDL2.SDL.SDL_Keycode.SDLK_s) Screenshot();
        if (key == SDL2.SDL.SDL_Keycode.SDLK_r) ToggleRecording();
        if (key == SDL2.SDL.SDL_Keycode.SDLK_t) Rotate();
        if (key == SDL2.SDL.SDL_Keycode.SDLK_f) Program.SdlCtx.SetFullScreen(!Program.SdlCtx.IsFullscreen);
        if (player!.Manager.AudioTarget is { } audio)
        {
            if (key == SDL2.SDL.SDL_Keycode.SDLK_UP) audio.Volume = Math.Min(1, audio.Volume + .1f);
            if (key == SDL2.SDL.SDL_Keycode.SDLK_DOWN) audio.Volume = Math.Max(0, audio.Volume - .1f);
        }
    }

    void Screenshot()
    {
        try
        {
            if (player?.Video is null) return;
            bool clipboard = Program.Options.Windows_ScreenToClip ^ Owner.ShiftDown;
            if (clipboard)
            {
                using var capture = SDLCapture.CaptureTexture(player.Video.TargetTexture);
                Platform.Specific.Win.WinClipboard.CopyCapture(capture);
                Notify("Screenshot copied", "To the clipboard", Good, 4, sidebar: true);
            }
            else
            {
                Directory.CreateDirectory(Program.Options.ScreenshotsPath);
                string file = Program.Options.GetFilePathForScreenshot();
                SDLCapture.ExportTexture(player.Video.TargetTexture, file);
                Notify("Screenshot saved", file, Good, 4, sidebar: true);
            }
            error = "";
        }
        catch (Exception ex) { error = "Screenshot failed: " + ex.Message; Notify("Screenshot failed", ex.Message, Bad, 6, sidebar: true); }
    }
    void Rotate() { player!.RotateClockwise(); rotation = (rotation + 1) % 4; }
    void ToggleRecording()
    {
        if (player is null) return;
        try
        {
            if (recorder is not null) { StopRecording(); return; }
            Directory.CreateDirectory(Program.Options.RecordingsPath);
            var manager = player.Manager;
            recordingPath = Program.Options.GetFilePathForVideo();
            recorder = new Mp4Output(recordingPath, manager.HasVideo ? new Mp4VideoTarget() : null, manager.HasAudio ? new Mp4AudioTarget() : null);
            recorder.Start(); manager.ChainTargets(recorder.VideoTarget, recorder.AudioTarget);
            recordingStart = ImGui.GetTime();
            Notify("Recording started", recordingPath, Bad, 4, sidebar: true);
        }
        catch (Exception ex) { recorder?.Dispose(); recorder = null; error = "Recording failed: " + ex.Message; Notify("Recording failed", ex.Message, Bad, 6, sidebar: true); }
    }
    void StopRecording()
    {
        var output = recorder; recorder = null;
        if (output is null) return;
        player!.Manager.UnchainTargets(output.VideoTarget, output.AudioTarget);
        try { output.Stop(); } finally { output.Dispose(); }
        Notify("Recording stopped", recordingPath, Warn, 4, sidebar: true);
    }
}
