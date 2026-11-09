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

// Same NAT-PMP protocol as PunchRouterMapping, applied to the actual browser
// listeners. A mapping is never evidence that an external browser connected.
internal sealed class BrowserPortMapping : IAsyncDisposable
{
    readonly UdpClient udp;
    readonly byte protocol;
    readonly ushort port;
    readonly CancellationTokenSource cancel = new();
    readonly SemaphoreSlim exchange = new(1);
    Task? renewing;
    bool owned;
    int renewalDelay = 60000;
    internal bool Active { get; private set; }
    internal ushort ExternalPort { get; private set; }
    internal BrowserPortMapping(IPEndPoint gateway, IPAddress local, int port, bool tcp)
    {
        if (port is < 1 or > 65535) throw new ArgumentException("Invalid browser port.");
        this.port = (ushort)port; ExternalPort = this.port; protocol = tcp ? (byte)2 : (byte)1;
        udp = new UdpClient(new IPEndPoint(local, 0)); udp.Connect(gateway);
    }
    internal static async Task<BrowserPortMapping?> Open(IPAddress local, int port, bool tcp, CancellationToken token)
    {
        var network = NetworkInterface.GetAllNetworkInterfaces().FirstOrDefault(n => n.OperationalStatus == OperationalStatus.Up && n.GetIPProperties().UnicastAddresses.Any(a => a.Address.Equals(local)));
        var gateway = network?.GetIPProperties().GatewayAddresses.FirstOrDefault(g => g.Address.AddressFamily == AddressFamily.InterNetwork)?.Address;
        if (gateway is null) return null;
        var mapping = new BrowserPortMapping(new IPEndPoint(gateway, 5351), local, port, tcp);
        try
        {
            if (!await mapping.Exchange(120, token)) { await mapping.DisposeAsync(); return null; }
            mapping.Active = true; mapping.renewing = mapping.Renew(); return mapping;
        }
        catch (Exception ex) when (ex is SocketException or OperationCanceledException or IOException)
        {
            await mapping.DisposeAsync(); token.ThrowIfCancellationRequested(); return null;
        }
    }
    internal async Task<bool> Exchange(uint seconds, CancellationToken token)
    {
        await exchange.WaitAsync(token);
        try
        {
            byte[] request = new byte[12]; request[1] = protocol;
            BinaryPrimitives.WriteUInt16BigEndian(request.AsSpan(4), port);
            BinaryPrimitives.WriteUInt16BigEndian(request.AsSpan(6), seconds == 0 ? (ushort)0 : ExternalPort);
            BinaryPrimitives.WriteUInt32BigEndian(request.AsSpan(8), seconds);
            await udp.SendAsync(request, token);
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token); timeout.CancelAfter(1000);
            while (true)
            {
                var reply = (await udp.ReceiveAsync(timeout.Token)).Buffer;
                if (reply.Length != 16 || reply[0] != 0 || reply[1] != protocol + 128 || BinaryPrimitives.ReadUInt16BigEndian(reply.AsSpan(8)) != port) continue;
                ushort assigned = BinaryPrimitives.ReadUInt16BigEndian(reply.AsSpan(10));
                bool accepted = BinaryPrimitives.ReadUInt16BigEndian(reply.AsSpan(2)) == 0 &&
                    (seconds == 0 || assigned != 0 && (!owned || assigned == ExternalPort) && BinaryPrimitives.ReadUInt32BigEndian(reply.AsSpan(12)) >= 60);
                if (accepted && seconds > 0) { ExternalPort = assigned; owned = true; }
                if (accepted && seconds > 0) renewalDelay = (int)Math.Min(60000, BinaryPrimitives.ReadUInt32BigEndian(reply.AsSpan(12)) * 500L);
                if (accepted && seconds == 0) owned = false;
                return accepted;
            }
        }
        finally { exchange.Release(); }
    }
    async Task Renew()
    {
        try { while (!cancel.IsCancellationRequested) { await Task.Delay(renewalDelay, cancel.Token); if (!await Exchange(120, cancel.Token)) break; } }
        catch (Exception ex) when (ex is SocketException or OperationCanceledException or IOException) { }
        finally { Active = false; }
    }
    public async ValueTask DisposeAsync()
    {
        cancel.Cancel(); if (renewing is not null) await renewing;
        using var deadline = new CancellationTokenSource(1000);
        try { if (owned) await Exchange(0, deadline.Token); } catch (Exception ex) when (ex is SocketException or OperationCanceledException or IOException) { }
        Active = false; udp.Dispose(); exchange.Dispose(); cancel.Dispose();
    }
}
