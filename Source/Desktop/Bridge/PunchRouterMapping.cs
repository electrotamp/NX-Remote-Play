using System;
using System.Buffers.Binary;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Threading;
using System.Threading.Tasks;

namespace NxRemotePlay.Client.Bridge;

// Mirrors the console's NAT-PMP adjunct: map the announced external port to a
// separate listener, accept only the announced peer, remove the mapping on exit.
internal sealed class PunchRouterMapping : IAsyncDisposable
{
    readonly UdpClient udp;
    readonly TcpListener listener;
    readonly ushort internalPort, externalPort;
    readonly CancellationTokenSource lifetime = new();
    Task? renewing;
    PunchRouterMapping(UdpClient udp, TcpListener listener, int externalPort)
    { this.udp = udp; this.listener = listener; internalPort = (ushort)((IPEndPoint)listener.LocalEndpoint).Port; this.externalPort = (ushort)externalPort; }
    internal static async Task<PunchRouterMapping?> Open(IPAddress local, int externalPort, CancellationToken token)
    {
        var network = NetworkInterface.GetAllNetworkInterfaces().FirstOrDefault(n => n.OperationalStatus == OperationalStatus.Up &&
            n.GetIPProperties().UnicastAddresses.Any(a => a.Address.Equals(local)));
        var gateway = network?.GetIPProperties().GatewayAddresses.FirstOrDefault(g => g.Address.AddressFamily == AddressFamily.InterNetwork)?.Address;
        if (gateway is null) return null;
        var listener = new TcpListener(local, 0); listener.Start(4);
        var udp = new UdpClient(new IPEndPoint(local, 0)); udp.Connect(gateway, 5351);
        var mapping = new PunchRouterMapping(udp, listener, externalPort);
        try
        {
            if (!await mapping.Exchange(120, token)) { await mapping.DisposeAsync(); return null; }
            mapping.renewing = mapping.Renew(); return mapping;
        }
        catch (Exception ex) when (ex is SocketException or OperationCanceledException or IOException)
        {
            await mapping.DisposeAsync(); if (token.IsCancellationRequested) token.ThrowIfCancellationRequested(); return null;
        }
    }
    async Task<bool> Exchange(uint seconds, CancellationToken token)
    {
        var request = new byte[12]; request[1] = 2;
        BinaryPrimitives.WriteUInt16BigEndian(request.AsSpan(4), internalPort);
        BinaryPrimitives.WriteUInt16BigEndian(request.AsSpan(6), seconds == 0 ? (ushort)0 : externalPort);
        BinaryPrimitives.WriteUInt32BigEndian(request.AsSpan(8), seconds);
        await udp.SendAsync(request, token);
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token); timeout.CancelAfter(1000);
        while (true)
        {
            var reply = (await udp.ReceiveAsync(timeout.Token)).Buffer;
            if (reply.Length != 16 || reply[0] != 0 || reply[1] != 130 || BinaryPrimitives.ReadUInt16BigEndian(reply.AsSpan(8)) != internalPort) continue;
            return BinaryPrimitives.ReadUInt16BigEndian(reply.AsSpan(2)) == 0 &&
                (seconds == 0 || BinaryPrimitives.ReadUInt16BigEndian(reply.AsSpan(10)) == externalPort && BinaryPrimitives.ReadUInt32BigEndian(reply.AsSpan(12)) > 0);
        }
    }
    async Task Renew()
    {
        try
        {
            while (!lifetime.IsCancellationRequested) { await Task.Delay(60000, lifetime.Token); if (!await Exchange(120, lifetime.Token)) break; }
        }
        catch (Exception ex) when (ex is SocketException or OperationCanceledException or IOException) { }
    }
    internal async Task<TcpClient> Accept(IPAddress peer, CancellationToken token)
    {
        while (true)
        {
            var socket = await listener.AcceptTcpClientAsync(token);
            if (((IPEndPoint)socket.Client.RemoteEndPoint!).Address.Equals(peer)) return socket;
            socket.Dispose();
        }
    }
    public async ValueTask DisposeAsync()
    {
        lifetime.Cancel(); listener.Stop();
        if (renewing is not null) await renewing;
        using var timeout = new CancellationTokenSource(1000);
        try { await Exchange(0, timeout.Token); } catch (Exception ex) when (ex is SocketException or OperationCanceledException or IOException) { }
        udp.Dispose(); lifetime.Dispose();
    }
}
