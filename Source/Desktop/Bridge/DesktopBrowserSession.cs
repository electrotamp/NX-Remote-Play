using Rendezvous;
using NxRemotePlay.Client.Core;
using NxRemotePlay.Client.Sources;
using System;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Threading;
using System.Threading.Tasks;

namespace NxRemotePlay.Client.Bridge;

internal sealed class DesktopBrowserSession : IAsyncDisposable
{
    readonly DesktopBrowserServer server;
    readonly BrowserRtcHost rtc;
    readonly StreamManager media;
    readonly bool ownsInput;
    readonly ViewerRegistry viewers;
    // False when the bridge session's registry is used: it must outlive this browser session and is disposed by its owner.
    readonly bool ownsViewers;
    readonly BrowserPortMapping? webMapping, mediaMapping;
    readonly CancellationTokenSource maintenanceCancel = new();
    Task? maintaining;
    Uri? publicLink;
    string certificateStatus = "";
    string? publicProblem;
    internal Uri? PublicLink => Volatile.Read(ref publicLink);
    internal BridgeInputArbiter Input { get; }
    internal Uri Link => server.JoinLink;
    // `link` (local or public address of this session) as the session link: joins without a password and stays valid for the whole USB session.
    internal Uri? SessionLink(Uri? link) => link is null ? null : NXRemotePlay.Protocol.BrowserJoinLink.CreateSession(link, server.SessionLinkToken);
    internal bool HasBrowser => rtc.HasParticipant;
    internal bool Active => server.Active && rtc.IsActive;
    // this session used to create its own registry even when a bridge session (native phones, remote browsers) was
    // running, so a phone and a local browser were both handed pad slot 1 and shared one controller (pad-flow log of 00:51:
    // two leases, both index 0 / slot 1). One registry must serve every viewer of one console.
    internal static ViewerRegistry ChooseRegistry(ViewerRegistry? shared, IInputSink source, BridgeInputArbiter input, out bool owns)
    {
        owns = shared is null;
        return shared ?? new ViewerRegistry(source, input);
    }
    DesktopBrowserSession(DesktopBrowserServer server, BrowserRtcHost rtc, ViewerRegistry viewers, bool ownsViewers, StreamManager media, BridgeInputArbiter input, bool ownsInput,
        Uri? publicLink, BrowserPortMapping? webMapping, BrowserPortMapping? mediaMapping, string certificateStatus)
    {
        this.server = server; this.rtc = rtc; this.viewers = viewers; this.ownsViewers = ownsViewers; this.media = media; Input = input; this.ownsInput = ownsInput;
        this.publicLink = publicLink; this.webMapping = webMapping; this.mediaMapping = mediaMapping;
        this.certificateStatus = certificateStatus;
        media.OnMediaPacket += rtc.Publish;
    }
    internal static IPAddress LocalAddress()
    {
        if (IPAddress.TryParse(AddressDetection.DetectLocalIp(), out var routed) && !IPAddress.IsLoopback(routed)) return routed;
        foreach (var network in NetworkInterface.GetAllNetworkInterfaces())
        {
            if (network.OperationalStatus != OperationalStatus.Up || network.NetworkInterfaceType is NetworkInterfaceType.Loopback or NetworkInterfaceType.Tunnel) continue;
            foreach (var entry in network.GetIPProperties().UnicastAddresses)
            {
                var ip = entry.Address;
                if (ip.AddressFamily != AddressFamily.InterNetwork) continue;
                var bytes = ip.GetAddressBytes();
                if (bytes[0] == 192 && bytes[1] == 168 || bytes[0] == 10 || bytes[0] == 172 && bytes[1] >= 16 && bytes[1] <= 31) return ip;
            }
        }
        return IPAddress.Loopback;
    }
    // A previous session on this port may still be closing (see DisposeAsync). Give it up to 3 s instead of failing at once.
    static async Task WaitForPort(int port, CancellationToken lifetime)
    {
        if (port <= 0) return;
        for (int attempt = 0; attempt < 15; attempt++)
        {
            try { var probe = new TcpListener(IPAddress.Any, port); probe.Start(); probe.Stop(); return; }
            catch (SocketException error) when (error.SocketErrorCode == SocketError.AddressAlreadyInUse)
            {
                if (attempt == 0) RtcFlowLog.Event("", "browser_port_wait", "port " + port + " still in use; waiting for the previous session to close");
                await Task.Delay(200, lifetime);
            }
        }
    }
    internal static async Task<DesktopBrowserSession> Start(IBridgeHostSource source, StreamManager media,
        int port, CancellationToken lifetime, BridgeInputArbiter? sharedInput = null, string rendezvous = "",
        bool publicSharing = false, bool routerMapping = false, int mediaPort = 8444, string? certificatePath = null,
        bool automaticCertificate = false, bool agreementAccepted = false, ViewerRegistry? sharedViewers = null)
    {
        if (publicSharing && (mediaPort is < 2 or > 65534 || mediaPort % 2 != 0)) throw new ArgumentException("Browser WebRTC needs an even UDP port from 2 to 65534.");
        IPAddress local = LocalAddress(); IPAddress? publicAddress = null;
        if (publicSharing) { string? detected = await AddressDetection.DetectPublicIp(lifetime); if (detected is not null) publicAddress = IPAddress.Parse(detected); }
        lifetime.ThrowIfCancellationRequested();
        string certificateStatus = string.IsNullOrWhiteSpace(certificatePath) ? "Self-signed HTTPS • Browser warning" : "Trusted public-IP certificate";
        if (publicAddress is not null && automaticCertificate)
        {
            BrowserPortMapping? challengeMapping = null;
            try
            {
                if (agreementAccepted && routerMapping && AcmeIpCertificate.NeedsRenewal(publicAddress))
                {
                    challengeMapping = await BrowserPortMapping.Open(local, 80, true, lifetime);
                    if (challengeMapping is not null && challengeMapping.ExternalPort != 80) throw new InvalidOperationException("Certificate validation requires public TCP 80; the router assigned a different port.");
                }
                certificatePath = await AcmeIpCertificate.Ensure(publicAddress, agreementAccepted, lifetime);
                certificateStatus = "Trusted public-IP certificate";
            }
            catch (Exception error) when (error is not OperationCanceledException)
            {
                certificateStatus = "HTTPS setup: " + error.Message;
                certificatePath = null;
            }
            finally { if (challengeMapping is not null) await challengeMapping.DisposeAsync(); }
        }
        await WaitForPort(port, lifetime);
        string[] urls = DiscoveryUrls(rendezvous);
        var server = new DesktopBrowserServer(source.SessionId, source.SessionPassword, local, port, publicAddress, urls.Length == 0 ? null : urls, certificatePath);
        var input = sharedInput ?? new BridgeInputArbiter(source, null, InputFlowLog.Open());
        var viewers = ChooseRegistry(sharedViewers, source, input, out bool ownsViewers);
        BrowserRtcHost? rtc = null;
        BrowserPortMapping? webMapping = null, mediaMapping = null;
        try
        {
            await server.Start(lifetime);
            using var signal = new BridgeSignalingClient(server.Address, server.CertificatePin);
            var socket = await signal.HostSocket(server.HostTicket(), lifetime);
            if (publicSharing && routerMapping && publicAddress is not null)
            {
                webMapping = await BrowserPortMapping.Open(local, port, true, lifetime);
                mediaMapping = await BrowserPortMapping.Open(local, mediaPort, false, lifetime);
            }
            RtcFlowLog.Event("", "host_start", $"local={local} public={publicAddress?.ToString() ?? "none"} webPort={port} webMapping={(webMapping is null ? "none" : webMapping.Active + " external " + webMapping.ExternalPort)} mediaPort={mediaPort} mediaMapping={(mediaMapping is null ? "none" : mediaMapping.Active + " external " + mediaMapping.ExternalPort)} ice={string.Join(" ", System.Linq.Enumerable.Select(server.IceServers, s => string.Join(" ", s.Urls)))} certificate={certificateStatus}");
            var configuration = BridgeIceConfiguration.Create(server.IceServers, false);
            configuration.X_BindAddress = local;
            rtc = new BrowserRtcHost(socket, configuration, lifetime, input, automaticControl: true,
                mediaPort: publicSharing ? mediaPort : 0, mappedCandidate: () => mediaMapping?.Active == true && publicAddress is not null ? new IPEndPoint(publicAddress, mediaMapping.ExternalPort) : null, registry: viewers);
            lifetime.ThrowIfCancellationRequested();
            var result = new DesktopBrowserSession(server, rtc, viewers, ownsViewers, media, input, sharedInput is null,
                publicAddress is null ? null : NXRemotePlay.Protocol.BrowserJoinLink.Create(publicAddress, webMapping?.Active == true ? webMapping.ExternalPort : port, source.SessionId), webMapping, mediaMapping, certificateStatus);
            if (publicAddress is not null) result.maintaining = result.Maintain(publicAddress, local, routerMapping, automaticCertificate, agreementAccepted, lifetime);
            return result;
        }
        catch
        {
            if (rtc is not null) await rtc.DisposeAsync();
            if (ownsViewers) await viewers.DisposeAsync();
            if (webMapping is not null) await webMapping.DisposeAsync();
            if (mediaMapping is not null) await mediaMapping.DisposeAsync();
            await server.DisposeAsync();
            if (sharedInput is null) await input.DisposeAsync();
            throw;
        }
    }
    async Task Maintain(IPAddress original, IPAddress local, bool routerMapping, bool automaticCertificate, bool agreementAccepted, CancellationToken lifetime)
    {
        using var linked = CancellationTokenSource.CreateLinkedTokenSource(lifetime, maintenanceCancel.Token);
        try
        {
            long nextCertificateAttempt = Environment.TickCount64 + 3600000;
            while (!linked.IsCancellationRequested)
            {
                await Task.Delay(TimeSpan.FromMinutes(5), linked.Token);
                string? current = await AddressDetection.DetectPublicIp(linked.Token);
                if (current is not null && current != original.ToString())
                {
                    Volatile.Write(ref publicLink, null);
                    Volatile.Write(ref publicProblem, "Public IP changed • Reconnect USB to update sharing"); return;
                }
                if (!automaticCertificate || !agreementAccepted || Environment.TickCount64 < nextCertificateAttempt || !AcmeIpCertificate.NeedsRenewal(original)) continue;
                nextCertificateAttempt = Environment.TickCount64 + 3600000;
                BrowserPortMapping? challengeMapping = null;
                try
                {
                    if (routerMapping)
                    {
                        challengeMapping = await BrowserPortMapping.Open(local, 80, true, linked.Token);
                        if (challengeMapping is not null && challengeMapping.ExternalPort != 80) throw new InvalidOperationException("Certificate validation requires public TCP 80; the router assigned a different port.");
                    }
                    string path = await AcmeIpCertificate.Ensure(original, true, linked.Token);
                    server.ReloadTrustedCertificate(path, original);
                    Volatile.Write(ref certificateStatus, "Trusted public-IP certificate");
                }
                catch (Exception error) when (error is not OperationCanceledException)
                {
                    Volatile.Write(ref certificateStatus, "HTTPS renewal needs attention: " + error.Message);
                }
                finally { if (challengeMapping is not null) await challengeMapping.DisposeAsync(); }
            }
        }
        catch (OperationCanceledException) when (linked.IsCancellationRequested) { }
        catch (Exception error) { Volatile.Write(ref publicProblem, "Public sharing check failed: " + error.Message); }
    }
    internal static string[] DiscoveryUrls(string rendezvous)
    {
        string host; int port;
        if (Uri.TryCreate(rendezvous, UriKind.Absolute, out var uri) && uri.Scheme == "https") { host = uri.Host; port = 7921; }
        else
        {
            int separator = rendezvous.LastIndexOf(':');
            if (separator <= 0 || !int.TryParse(rendezvous[(separator + 1)..], out port) || port is < 1 or > 65535) return [];
            host = rendezvous[..separator];
        }
        string url = "stun:" + host + ":" + port;
        _ = BridgeIceConfiguration.Create([new NXRemotePlay.Protocol.RtcIceServer([url])], false);
        return [url];
    }
    // The shutdown used to be one await chain (maintenance, WebRTC peers, signalling server). After a USB stall a
    // viewer peer's dispose never returned, so the server step was never reached: the HTTPS listener stayed on the port for ever, the
    // 3-second Disconnect budget dropped the reference, and the next session failed with "address already in use". The listener is now
    // released FIRST, and every later step is bounded so one stuck step cannot keep the port or stall the others.
    int disposed;
    static async Task Bounded(Func<Task> step, int milliseconds, string name)
    {
        try
        {
            var task = Task.Run(step);
            if (await Task.WhenAny(task, Task.Delay(milliseconds)) != task)
            {
                RtcFlowLog.Event("", "browser_dispose_timeout", name + " did not finish in " + milliseconds + " ms; continuing");
                _ = task.ContinueWith(t => _ = t.Exception, TaskScheduler.Default);
            }
            else await task;
        }
        catch (Exception error) { RtcFlowLog.Event("", "browser_dispose_error", name + ": " + error.GetType().Name + " " + error.Message); }
    }
    public async ValueTask DisposeAsync()
    {
        if (Interlocked.Exchange(ref disposed, 1) != 0) return;
        maintenanceCancel.Cancel();
        media.OnMediaPacket -= rtc.Publish;
        await Bounded(() => server.DisposeAsync().AsTask(), 3000, "signalling server");
        if (maintaining is not null) await Bounded(() => maintaining, 1500, "maintenance");
        await Bounded(async () => { await rtc.DisposeAsync(); if (ownsViewers) await viewers.DisposeAsync(); }, 3000, "WebRTC host");
        if (webMapping is not null) await Bounded(() => webMapping.DisposeAsync().AsTask(), 2000, "web port mapping");
        if (mediaMapping is not null) await Bounded(() => mediaMapping.DisposeAsync().AsTask(), 2000, "media port mapping");
        if (ownsInput) await Bounded(() => Input.DisposeAsync().AsTask(), 2000, "input arbiter");
        if (maintaining is null || maintaining.IsCompleted) maintenanceCancel.Dispose();
    }
}
