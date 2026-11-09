using NxRemotePlay.Client.Core;
using NxRemotePlay.Client.Sources;
using SIPSorcery.Net;
using NXRemotePlay.Protocol;
using System;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Threading;
using System.Threading.Tasks;

namespace NxRemotePlay.Client.Bridge;

// Remote-route lifetime is independent of the local player. This coordinator subscribes
// to the player's sole USB reader; its network retries never cancel local capture.
internal sealed class DesktopBridgeSession : IAsyncDisposable
{
    readonly IBridgeHostSource source;
    readonly StreamManager local;
    readonly IBridgeOwnerClient signal;
    readonly BridgeSignalingClient? https;
    readonly NativeForwarder native;
    readonly NativeLanDiscovery lan;
    readonly UdpmViewerPlace? udpm;
    BrowserPortMapping? udpMapping;
    readonly BridgeLease lease;
    readonly CancellationTokenSource cancel;
    readonly RTCConfiguration ice;
    readonly Task running, heartbeats;
    BrowserRtcHost? rtc;
    DesktopPunchHost? punch;
    long expires;
    long lastMedia;
    bool online;
    internal readonly BridgeInputArbiter Input;
    readonly ViewerRegistry viewers;
    // The one registry for every viewer of this console (native, remote browsers, and the desktop's own local browser session).
    internal ViewerRegistry Viewers => viewers;
    internal event Action<string>? Status;
    internal bool Online => Volatile.Read(ref online) && DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() < Interlocked.Read(ref expires);
    internal bool HasNative => native.ViewerCount != 0;
    // Remote viewers connected right now (native and browser together, at most ViewerLimits.MaxRemoteViewers).
    internal int ViewerCount => viewers.Count;

    internal DesktopBridgeSession(IBridgeHostSource source, StreamManager local, string server,
        IPAddress advertisedAddress, RTCConfiguration ice, CancellationToken lifetime, string? certificatePin = null, int lanPort = 9913, int udpmPort = UdpmViewerPlace.DefaultPort)
    {
        if (!source.SupportsDelegation || !source.SupportsOwnerProof) throw new InvalidOperationException("Switch ownership recovery is not configured.");
        if (advertisedAddress.Equals(IPAddress.Any) || advertisedAddress.Equals(IPAddress.IPv6Any)) throw new ArgumentException("Missing bridge address.");
        this.source = source; this.local = local; this.ice = ice;
        cancel = CancellationTokenSource.CreateLinkedTokenSource(lifetime);
        try
        {
            if (Uri.TryCreate(server, UriKind.Absolute, out var secure) && secure.Scheme == "https") signal = https = new BridgeSignalingClient(secure, certificatePin);
            else signal = new NativeOwnerClient(server, source.RequestOwnerProof);
            Input = new BridgeInputArbiter(source, null, InputFlowLog.Open());
            viewers = new ViewerRegistry(source, Input);
            // The same UDP video as a console hosting itself: one native viewer at a time (negative port = TCP video only).
            if (udpmPort >= 0) udpm = UdpmViewerPlace.TryOpen(udpmPort, message => Status?.Invoke(message));
            native = new NativeForwarder(source.SessionId, source.SessionPassword, IPAddress.Any, authorized: () => Online, controls: Input, registry: viewers, udpm: udpm);
            native.Start();
            if (udpm is not null) _ = MapUdpm(advertisedAddress);
            lan = new NativeLanDiscovery(source.SessionId, native, () => Online, cancel.Token, lanPort);
            var address = new IPEndPoint(advertisedAddress, native.Endpoint.Port);
            string endpoint = new NativeBridgeRoute(address,
                NativeBridgeRoute.HashPassword(source.SessionId, source.SessionPassword)).ToString();
            lease = new BridgeLease(signal, source.SessionId, "desktop-" + Guid.NewGuid().ToString("N"), endpoint, source.RequestOwnerProof, source.RequestDelegation);
            local.OnMediaPacket += Publish;
            lastMedia = Environment.TickCount64;
            running = Task.Run(Run); heartbeats = Task.Run(Heartbeat);
        }
        catch
        {
            local.OnMediaPacket -= Publish;
            if (lan is not null) lan.DisposeAsync().AsTask().GetAwaiter().GetResult();
            if (native is not null) native.DisposeAsync().AsTask().GetAwaiter().GetResult();
            if (udpm is not null) udpm.DisposeAsync().AsTask().GetAwaiter().GetResult();
            if (viewers is not null) viewers.DisposeAsync().AsTask().GetAwaiter().GetResult();
            if (Input is not null) Input.DisposeAsync().AsTask().GetAwaiter().GetResult();
            signal?.Dispose(); cancel.Dispose(); throw;
        }
    }
    // Router-visible UDP port for viewers outside the LAN (NAT-PMP, same approach as the browser media port).
    async Task MapUdpm(IPAddress local)
    {
        try
        {
            var mapping = await BrowserPortMapping.Open(local, udpm!.Port, false, cancel.Token);
            if (mapping is null) return;
            if (cancel.IsCancellationRequested) { await mapping.DisposeAsync(); return; }
            udpMapping = mapping; udpm.ExternalPort = mapping.ExternalPort;
        }
        catch (Exception) { /* LAN viewers need no mapping; outside viewers keep TCP video */ }
    }
    void Publish(ReceivedPacket packet)
    {
        if (!packet.Header.IsError) Interlocked.Exchange(ref lastMedia, Environment.TickCount64);
        if (!Online) return;
        native.Publish(packet); Volatile.Read(ref rtc)?.Publish(packet);
    }
    async Task Heartbeat()
    {
        try
        {
            while (!cancel.IsCancellationRequested)
            {
                await Task.Delay(4000, cancel.Token);
                if (Environment.TickCount64 - Interlocked.Read(ref lastMedia) > 5000)
                {
                    Volatile.Write(ref online, false); native.DisconnectViewer(); Status?.Invoke("Bridge paused: USB media unavailable");
                    continue;
                }
                // The Switch maintains its local attachment deadline only while
                // media is moving. Do not send a second USB heartbeat here.
            }
        }
        catch (OperationCanceledException) when (cancel.IsCancellationRequested) { }
        catch (Exception) { Volatile.Write(ref online, false); cancel.Cancel(); Status?.Invoke("USB Viewer: bridge acknowledgement lost"); }
    }
    async Task ClearRemote()
    {
        Volatile.Write(ref online, false); native.DisconnectViewer();
        var oldPunch = Interlocked.Exchange(ref punch, null);
        if (oldPunch is not null) await oldPunch.DisposeAsync();
        var previous = Interlocked.Exchange(ref rtc, null);
        if (previous is not null) try { await previous.DisposeAsync(); } catch (Exception) { }
        try { await Input.Revoke(); } catch (Exception) { }
    }
    async Task Run()
    {
        try
        {
            while (!cancel.IsCancellationRequested)
            {
                try
                {
                    Status?.Invoke("USB Viewer: connecting bridge");
                    if (Environment.TickCount64 - Interlocked.Read(ref lastMedia) > 5000) throw new IOException("USB media unavailable.");
                    var route = await lease.Execute(0, cancel.Token);
                    BrowserRtcHost? next = null;
                    if (https is not null)
                    {
                        var socket = await https.HostSocket(route.SocketToken ?? throw new IOException("Missing host signaling ticket."), cancel.Token);
                        next = new BrowserRtcHost(socket, route.IceServers is null ? ice : BridgeIceConfiguration.Create(route.IceServers, route.RelayOnly), cancel.Token, Input, automaticControl: true, registry: viewers);
                        Interlocked.Exchange(ref rtc, next);
                    }
                    Interlocked.Exchange(ref expires, route.ExpiresAt); Volatile.Write(ref online, true);
                    punch = new DesktopPunchHost(signal.ServerHost, () => lease.Route, () => Online, native, cancel.Token);
                    Status?.Invoke("Bridge Mode • " + source.SessionId);
                    while (!cancel.IsCancellationRequested)
                    {
                        await Task.Delay(4000, cancel.Token);
                        if (next is not null && !next.IsActive) throw new IOException("Bridge signaling disconnected.");
                        if (Environment.TickCount64 - Interlocked.Read(ref lastMedia) > 5000) throw new IOException("USB media unavailable.");
                        route = await lease.Execute(1, cancel.Token);
                        Interlocked.Exchange(ref expires, route.ExpiresAt);
                    }
                }
                catch (OperationCanceledException) when (cancel.IsCancellationRequested) { break; }
                catch (Exception error)
                {
                    await ClearRemote(); Status?.Invoke("Native hosting unavailable: " + error.Message);
                    // An uncertain server lease must expire before a new claim. The
                    // Switch's local attachment suppression expires independently if
                    // its USB media path stops making progress.
                    await Task.Delay(16000, cancel.Token);
                }
            }
        }
        catch (OperationCanceledException) when (cancel.IsCancellationRequested) { }
        finally
        {
            await ClearRemote();
            if (lease.Route is not null)
            {
                using var release = new CancellationTokenSource(1500); // best effort: an unreleased lease simply expires on the server
                try { await lease.Execute(2, release.Token); } catch (Exception) { }
            }
        }
    }
    public async ValueTask DisposeAsync()
    {
        cancel.Cancel();
        try { await Task.WhenAll(running, heartbeats); }
        finally
        {
            local.OnMediaPacket -= Publish;
            await lan.DisposeAsync(); await native.DisposeAsync();
            if (udpMapping is not null) await udpMapping.DisposeAsync();
            if (udpm is not null) await udpm.DisposeAsync();
            await viewers.DisposeAsync(); await Input.DisposeAsync();
            signal.Dispose(); cancel.Dispose();
        }
    }
}
