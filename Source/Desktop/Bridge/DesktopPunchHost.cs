using NXRemotePlay.Protocol;
using NxRemotePlay.Client.Sources;
using System;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Threading;
using System.Threading.Tasks;

namespace NxRemotePlay.Client.Bridge;

// Same local port is used for the server registration and direct peer connect.
// The server carries only endpoint exchange; native media uses the resulting socket.
internal sealed class DesktopPunchHost : IAsyncDisposable
{
    readonly CancellationTokenSource cancel;
    readonly Task running;
    internal DesktopPunchHost(string server, Func<BridgeRouteReply?> current, Func<bool> online,
        NativeForwarder native, CancellationToken lifetime)
    {
        cancel = CancellationTokenSource.CreateLinkedTokenSource(lifetime);
        running = Task.Run(async () =>
        {
            string? used = null;
            try
            {
                while (!cancel.IsCancellationRequested)
                {
                    var route = current();
                    if (!online() || route?.PunchTicket is null || route.PunchTicket == used || !native.HasRoom)
                    {
                        await Task.Delay(100, cancel.Token); continue;
                    }
                    used = route.PunchTicket;
                    try { await Attempt(server, route, native, cancel.Token); }
                    catch (Exception ex) when (ex is IOException or SocketException or OperationCanceledException)
                    {
                        if (cancel.IsCancellationRequested) break;
                        await Task.Delay(250, cancel.Token);
                    }
                }
            }
            catch (OperationCanceledException) when (cancel.IsCancellationRequested) { }
        });
    }
    internal static async Task<TcpClient> Open(string host, int port, int localPort, CancellationToken token)
    {
        var socket = new TcpClient(AddressFamily.InterNetwork) { NoDelay = true };
        try
        {
            socket.Client.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
            socket.Client.Bind(new IPEndPoint(IPAddress.Any, localPort));
            await socket.ConnectAsync(host, port, token); return socket;
        }
        catch { socket.Dispose(); throw; }
    }
    internal static async Task Attempt(string server, BridgeRouteReply route, NativeForwarder native, CancellationToken token)
    {
        if (route.PunchPort is < 1 or > 65535 || route.PunchTicket?.Length != 64) throw new IOException("Invalid P2P host ticket.");
        int port = RandomNumberGenerator.GetInt32(20000, 60000);
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(token);
        deadline.CancelAfter(60000);
        IPEndPoint peer;
        PunchRouterMapping? mapping = null;
        try
        {
            using (var signal = await Open(server, route.PunchPort, port, deadline.Token))
            {
                await PunchSource.WriteLine(signal.GetStream(), $"BRIDGEHOST {route.Id} {route.PunchTicket}", deadline.Token);
                string registered = await PunchSource.ReadLine(signal.GetStream(), deadline.Token);
                if (!registered.StartsWith("OK ") || !IPEndPoint.TryParse(registered[3..], out var observed)) throw new IOException("P2P host registration rejected.");
                mapping = await PunchRouterMapping.Open(((IPEndPoint)signal.Client.LocalEndPoint!).Address, observed.Port, deadline.Token);
                string reply = await PunchSource.ReadLine(signal.GetStream(), deadline.Token);
                if (!reply.StartsWith("PEER ") || !IPEndPoint.TryParse(reply[5..], out peer!)) throw new IOException("Invalid P2P peer endpoint.");
            }
            using var connect = CancellationTokenSource.CreateLinkedTokenSource(token); connect.CancelAfter(15000);
            var outgoing = Open(peer.Address.ToString(), peer.Port, port, connect.Token);
            TcpClient socket;
            if (mapping is null) socket = await outgoing;
            else
            {
                var incoming = mapping.Accept(peer.Address, connect.Token);
                var first = await Task.WhenAny(outgoing, incoming);
                // A refused punch must not discard a mapping which can still accept.
                try { socket = await first; }
                catch (Exception ex) when (ex is SocketException or IOException) { socket = await (first == outgoing ? incoming : outgoing); }
                connect.Cancel();
                var other = first == outgoing ? incoming : outgoing;
                if (first.IsFaulted) other = first;
                try { var extra = await other; if (!ReferenceEquals(extra, socket)) extra.Dispose(); } catch (Exception ex) when (ex is SocketException or IOException or OperationCanceledException) { }
            }
            // Multi-viewer: the session continues on its own; this attempt is finished once the connection is handed over, so the
            // host can arm the next attempt (another viewer) while this one streams. The router mapping must outlive the session,
            // as it always did, so it is released when the session ends.
            var session = native.AcceptPunched(socket);
            if (mapping is not null)
            {
                var held = mapping; mapping = null;
                _ = session.ContinueWith(_ => held.DisposeAsync().AsTask(), CancellationToken.None, TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default).Unwrap();
            }
        }
        finally { if (mapping is not null) await mapping.DisposeAsync(); }
    }
    public async ValueTask DisposeAsync() { cancel.Cancel(); try { await running; } finally { cancel.Dispose(); } }
}
