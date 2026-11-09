using NxRemotePlay.Client.Core;
using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Net.Security;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Security.Authentication;
using System.Text;
using System.Buffers.Binary;
using System.Threading;
using System.Threading.Channels;
using System.Threading.Tasks;
using NXRemotePlay.Protocol;
using NxRemotePlay.Client.Bridge;

namespace NxRemotePlay.Client.Sources;

// Uses the shared session-pairing protocol.
internal sealed class PunchSource(string server, int port, string id, string password,
    StreamingOptions options, CancellationToken cancellation) : StreamingSource(options, cancellation), IInputSink
{
    public bool SupportsInput { get; private set; } = true;
    TcpClient? client;
    Stream Stream => client!.GetStream();
    readonly SemaphoreSlim writes = new(1);
    byte[]? connTicket; // 6-byte connection ticket from PUNCHJOIN2, sent in the handshake request's Reserved bytes
    public string Route { get; private set; } = "";

    // UDP media (UDPM) is the primary transport: video and audio arrive as UDPM frames on one socket and TCP only carries
    // input and the console's liveness. A console that does not offer UDP (or a failed bind) keeps the TCP media path.
    const byte FeatureDeliveryFeedback = 0x10, FeatureUdpm = 0x20, FeatureUdpmNat = 0x40;
    const uint DeliveryFeedbackAck = 0x3346514e;
    UdpmClientChannel? udp;
    Channel<byte[]>? udpQueue;
    byte[]? handshakeResponse;
    internal async Task ConnectDirect(IPEndPoint endpoint)
    {
        ValidateCredentials(id, password);
        try
        {
            client = await Open(endpoint.Address.ToString(), endpoint.Port, 5000);
            await Authenticate(client.GetStream(), password, Cancellation);
            Route = "Local network";
            await DoHandshake(StreamKind.Both);
            StartUdp();
        }
        catch { Dispose(); throw; }
    }
    internal static void ValidateCredentials(string id, string password)
    {
        if (id.Length != 5 || password.Length != 5 ||
            !System.Linq.Enumerable.All(id + password, char.IsAsciiLetterOrDigit))
            throw new ArgumentException("Enter the five-character Session ID and Password shown on the Switch.");
    }

    public override Task Connect() => ConnectNative();

    internal async Task ConnectNative(bool discoverLan = true)
    {
        ValidateCredentials(id, password);
        try
        {
            ReportMessage("Looking for the Switch on this network...");
            var endpoint = discoverLan ? await Discover() : null;
            if (endpoint is not null)
            {
                client = await Open(endpoint.Address.ToString(), endpoint.Port, 5000);
                await Authenticate(client.GetStream(), password, Cancellation);
                Route = "Local network";
            }
            else
            {
                ReportMessage("Waking the Switch...");
                using (var udp = new UdpClient())
                {
                    IgnoreUdpUnreachable(udp);
                    var addresses = await Dns.GetHostAddressesAsync(server, AddressFamily.InterNetwork, Cancellation);
                    if (addresses.Length == 0) throw new IOException("The server has no IPv4 address.");
                    udp.Connect(addresses[0], port);
                    await udp.SendAsync(Encoding.ASCII.GetBytes($"PUNCHWAKE {id} {password}\n"), Cancellation);
                    using var wake = CancellationTokenSource.CreateLinkedTokenSource(Cancellation);
                    wake.CancelAfter(3000);
                    try
                    {
                        var response = await udp.ReceiveAsync(wake.Token);
                        if (Encoding.ASCII.GetString(response.Buffer).Trim() == "ERR NOTFOUND")
                            throw new IOException("No Switch is hosting with that ID and password.");
                    }
                    catch (OperationCanceledException) when (!Cancellation.IsCancellationRequested) { }
                }
                int localPort = 0;
                var deadline = DateTime.UtcNow.AddSeconds(45);
                string reply = "";
                bool ticketJoin = true; // PUNCHJOIN2 first; an older server answers ERR BADCMD and we retry with PUNCHJOIN
                connTicket = null;
                do
                {
                    // a fresh local port for every attempt. Windows refuses to connect again from the same
                    // port to the same server while the previous attempt's socket sits in TIME_WAIT (WSAEADDRINUSE),
                    // which broke every join that needed a retry (the Switch needs up to 30 s to answer the wake).
                    // Only the port of the attempt that succeeds matters: the punch reuses it.
                    // A port Windows has reserved (excluded ranges) or another program owns fails to bind with
                    // AccessDenied/AddressInUse: that only means "pick another port", never "the join failed".
                    TcpClient? opened = null;
                    for (int tries = 0; opened is null; tries++)
                    {
                        int previousPort = localPort;
                        do localPort = RandomNumberGenerator.GetInt32(20000, 60000); while (localPort == previousPort);
                        try { opened = await Open(server, port, 10000, localPort); }
                        catch (SocketException ex) when (tries < 20 && ex.SocketErrorCode is SocketError.AccessDenied or SocketError.AddressAlreadyInUse) { }
                    }
                    using var signal = opened;
                    await WriteLine(signal.GetStream(), $"{(ticketJoin ? "PUNCHJOIN2" : "PUNCHJOIN")} {id} {password}", Cancellation);
                    reply = await ReadLine(signal.GetStream(), Cancellation);
                    if (ticketJoin && reply == "ERR BADCMD") { ticketJoin = false; continue; }
                    if (reply != "ERR NOTFOUND") break;
                    ReportMessage("Waiting for the Switch to respond...");
                    await Task.Delay(1500, Cancellation);
                } while (DateTime.UtcNow < deadline);
                // "OK <ip:port>" or, with a connection ticket, "OK <ip:port> <12 hex>" (PUNCHJOIN2 only).
                var replyTokens = reply.StartsWith("OK ") ? reply[3..].Split(' ', StringSplitOptions.RemoveEmptyEntries) : Array.Empty<string>();
                if (replyTokens.Length == 0 || !IPEndPoint.TryParse(replyTokens[0], out var peer))
                    throw new IOException("The Switch did not answer. Check that Hosting is enabled and it is available.");
                if (replyTokens.Length > 1 && replyTokens[1].Length == 12 && IsHex(replyTokens[1]))
                    connTicket = Convert.FromHexString(replyTokens[1]);
                ReportMessage("Connecting to the Switch...");
                client = await Open(peer.Address.ToString(), peer.Port, 15000, localPort);
                Route = "Internet";
            }
            await DoHandshake(StreamKind.Both);
            StartUdp();
        }
        catch { Dispose(); throw; }
    }

    async Task<TcpClient> Open(string host, int targetPort, int timeout, int localPort = 0)
    {
        var socket = new TcpClient(AddressFamily.InterNetwork);
        try
        {
            socket.NoDelay = true;
            if (localPort != 0)
            {
                socket.Client.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
                socket.Client.Bind(new IPEndPoint(IPAddress.Any, localPort));
            }
            using var limit = CancellationTokenSource.CreateLinkedTokenSource(Cancellation);
            limit.CancelAfter(timeout);
            await socket.ConnectAsync(host, targetPort, limit.Token);
            return socket;
        }
        catch { socket.Dispose(); throw; }
    }

    // on Windows a UDP socket that sent to a host answering "port unreachable" (ICMP) makes the next
    // ReceiveAsync throw SocketException 10054 "An existing connection was forcibly closed by the remote host".
    // The LAN search sweeps the whole /24, so any machine without a listener on 9913 broke every desktop Join
    // that searched the local network first. SIO_UDP_CONNRESET off turns that behaviour off.
    internal static void IgnoreUdpUnreachable(UdpClient udp)
    {
        if (!OperatingSystem.IsWindows()) return;
        try { udp.Client.IOControl(unchecked((int)0x9800000C), new byte[] { 0 }, null); } catch { }
    }

    internal async Task<IPEndPoint?> Discover()
    {
        using var udp = new UdpClient(AddressFamily.InterNetwork) { EnableBroadcast = true };
        IgnoreUdpUnreachable(udp);
        var broadcasts = new HashSet<IPAddress> { IPAddress.Broadcast };
        var sweep = new HashSet<IPAddress>();
        foreach (var nic in NetworkInterface.GetAllNetworkInterfaces())
        {
            if (nic.OperationalStatus != OperationalStatus.Up || nic.NetworkInterfaceType == NetworkInterfaceType.Loopback) continue;
            foreach (var address in nic.GetIPProperties().UnicastAddresses)
            {
                if (address.Address.AddressFamily != AddressFamily.InterNetwork || address.IPv4Mask is null) continue;
                var ip = address.Address.GetAddressBytes();
                var mask = address.IPv4Mask.GetAddressBytes();
                var broadcast = new byte[4];
                for (int i = 0; i < 4; i++) broadcast[i] = (byte)(ip[i] | ~mask[i]);
                broadcasts.Add(new IPAddress(broadcast));
                // Match Android's bounded /24 fallback, restricted to our actual subnet.
                for (int host = 1; host < 255; host++)
                    if (host != ip[3] && (host & mask[3]) == (ip[3] & mask[3]))
                        sweep.Add(new IPAddress(new byte[] { ip[0], ip[1], ip[2], (byte)host }));
            }
        }
        var message = Encoding.ASCII.GetBytes($"NXLAN1 DISCOVER {id}\n");
        async Task Send(IPAddress address)
        {
            try { await udp.SendAsync(message, new IPEndPoint(address, 9913), Cancellation); }
            catch (SocketException) { }
        }
        async Task<IPEndPoint?> Receive(int milliseconds)
        {
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(Cancellation);
            timeout.CancelAfter(milliseconds);
            try
            {
                while (true)
                {
                    UdpReceiveResult packet;
                    // belt and braces for IgnoreUdpUnreachable; a stray ICMP error must never end the search.
                    try { packet = await udp.ReceiveAsync(timeout.Token); }
                    catch (SocketException) { continue; }
                    var reply = Encoding.ASCII.GetString(packet.Buffer).Trim();
                    if (reply == $"NXLAN1 BUSY {id}") throw new IOException("The Switch is already connected to another device.");
                    string prefix = $"NXLAN1 HERE {id} ";
                    if (reply.StartsWith(prefix) && int.TryParse(reply[prefix.Length..], out var hostPort) && hostPort is > 0 and <= 65535)
                        return new IPEndPoint(packet.RemoteEndPoint.Address, hostPort);
                }
            }
            catch (OperationCanceledException) when (!Cancellation.IsCancellationRequested) { return null; }
        }
        for (int round = 0; round < 6; round++)
        {
            foreach (var address in broadcasts) await Send(address);
            if (await Receive(250) is { } found) return found;
        }
        foreach (var address in sweep) await Send(address);
        return await Receive(1500);
    }

    internal static async Task Authenticate(Stream stream, string password, CancellationToken token)
    {
        string challenge = await ReadLine(stream, token);
        const string prefix = "NXLAN1 CHALLENGE ";
        if (!challenge.StartsWith(prefix) || challenge.Length != prefix.Length + 16 ||
            !System.Linq.Enumerable.All(challenge[prefix.Length..], char.IsAsciiHexDigit))
            throw new IOException("The Switch returned an invalid authentication challenge.");
        string nonce = Convert.ToHexString(RandomNumberGenerator.GetBytes(8)).ToLowerInvariant();
        string hash = Convert.ToHexString(SHA256.HashData(Encoding.ASCII.GetBytes($"{password}:{challenge[prefix.Length..]}:{nonce}"))).ToLowerInvariant();
        await WriteLine(stream, $"NXLAN1 AUTH {nonce} {hash}", token);
        if (await ReadLine(stream, token) != "NXLAN1 OK") throw new IOException("The Switch password was incorrect.");
    }

    internal static async Task<string> ReadLine(Stream stream, CancellationToken token)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token);
        timeout.CancelAfter(10000);
        var bytes = new List<byte>();
        byte[] single = new byte[1];
        while (bytes.Count < 256)
        {
            if (await stream.ReadAsync(single, timeout.Token) == 0) throw new EndOfStreamException("The connection closed.");
            if (single[0] == 10) return Encoding.ASCII.GetString(bytes.ToArray()).TrimEnd('\r');
            bytes.Add(single[0]);
        }
        throw new IOException("The server response was too long.");
    }

    static bool IsHex(string text)
    {
        foreach (char c in text) if (!Uri.IsHexDigit(c)) return false;
        return true;
    }
    internal static async Task WriteLine(Stream stream, string line, CancellationToken token) =>
        await stream.WriteAsync(Encoding.ASCII.GetBytes(line + "\n"), token);

    async Task<byte[]> ReadExact(int size)
    {
        var bytes = new byte[size];
        await Stream.ReadExactlyAsync(bytes, Cancellation);
        return bytes;
    }

    protected override Task<byte[]> ReadHandshakeHello(StreamKind stream, int maxBytes) => ReadExact(maxBytes);
    protected override async Task<byte[]> SendHandshakePacket(ProtoHandshakeRequest req, int expectedLength)
    {
        byte[] bytes = new byte[ProtoHandshakeRequest.StructureSize];
        MemoryMarshal.Write(bytes, in req);
        // connection ticket from PUNCHJOIN2 in the Reserved bytes (offsets 10..15): the console then
        // accepts this join even when NAT changed its source address (a join from inside its own house).
        if (connTicket is { Length: 6 }) Array.Copy(connTicket, 0, bytes, 10, 6);
        // Ask for UDP media; the NAT bit only with a rendezvous address to register at.
        var rendezvous = await ResolveRendezvous();
        bytes[9] |= (byte)(FeatureDeliveryFeedback | FeatureUdpm | (rendezvous is not null ? FeatureUdpmNat : 0));
        await Stream.WriteAsync(bytes, Cancellation);
        handshakeResponse = await ReadExact(expectedLength);
        rendezvousEndpoint = rendezvous;
        return handshakeResponse;
    }

    IPEndPoint? rendezvousEndpoint;
    async Task<IPEndPoint?> ResolveRendezvous()
    {
        if (string.IsNullOrWhiteSpace(server) || port is <= 0 or > 65535) return null;
        try
        {
            var addresses = await Dns.GetHostAddressesAsync(server, AddressFamily.InterNetwork, Cancellation);
            return addresses.Length > 0 ? new IPEndPoint(addresses[0], port) : null;
        }
        catch (Exception ex) when (ex is SocketException or ArgumentException) { return null; }
    }

    // Called once the handshake is done: starts the UDP receiver when the console offered one.
    void StartUdp()
    {
        var response = handshakeResponse; handshakeResponse = null;
        if (response is not { Length: ProtoHandshakeResponse03.Size } ||
            BinaryPrimitives.ReadUInt32LittleEndian(response.AsSpan(4)) != DeliveryFeedbackAck) return;
        ulong offer = BinaryPrimitives.ReadUInt64LittleEndian(response.AsSpan(8));
        if ((offer >> 63) == 0 || client?.Client.RemoteEndPoint is not IPEndPoint console)
        {
            ReportMessage("The Switch did not offer UDP media; using the TCP connection.");
            return;
        }
        int udpPort = (int)((offer >> 32) & 0xFFFF);
        uint token = (uint)offer;
        byte[]? natKey = BinaryPrimitives.ReadUInt32LittleEndian(response.AsSpan(32)) == UdpmClientChannel.NatMagic ? response[16..32] : null;
        var queue = Channel.CreateBounded<byte[]>(new BoundedChannelOptions(512) { FullMode = BoundedChannelFullMode.DropWrite, SingleReader = true });
        udpQueue = queue;
        udp = new UdpmClientChannel(console.Address, udpPort, token, natKey, rendezvousEndpoint,
            wire => { if (!queue.Writer.TryWrite(wire)) Program.DebugLog("UDPM media: receive queue full, packet dropped"); },
            wire => { if (!queue.Writer.TryWrite(wire)) Program.DebugLog("UDPM media: receive queue full, audio dropped"); },
            why => queue.Writer.TryComplete(new IOException(why)),
            message => Program.DebugLog(message)).Start();
        // The TCP connection only has to stay open; media records that arrive on it are ignored. Its end ends the session.
        _ = Task.Run(async () =>
        {
            try
            {
                while (!Cancellation.IsCancellationRequested)
                {
                    var header = ParseHeader(await ReadExact(PacketHeader.StructLength));
                    await ReadExact(header.DataSize);
                }
            }
            catch (Exception) { queue.Writer.TryComplete(new IOException("The connection to the Switch ended.")); }
        });
        ReportMessage("Media over UDP");
    }

    internal static PacketHeader ParseHeader(byte[] bytes)
    {
        if (bytes.Length != PacketHeader.StructLength) throw new IOException("Invalid media header length.");
        var header = MemoryMarshal.Read<PacketHeader>(bytes);
        if (header.Magic != PacketHeader.MagicResponse || header.DataSize < 0 || header.DataSize > StreamInfo.MaxPayloadSize)
            throw new IOException("Invalid media packet.");
        return header;
    }
    public override async Task<ReceivedPacket> ReadNextPacket()
    {
        if (udpQueue is not null)
        {
            var wire = await udpQueue.Reader.ReadAsync(Cancellation);
            var udpHeader = ParseHeader(wire[..PacketHeader.StructLength]);
            if (udpHeader.DataSize != wire.Length - PacketHeader.StructLength) throw new IOException("Invalid media packet.");
            var udpBuffer = PoolBuffer.Rent(udpHeader.DataSize);
            wire.AsMemory(PacketHeader.StructLength).CopyTo(udpBuffer.Memory);
            return new(udpHeader, udpBuffer);
        }
        var header = ParseHeader(await ReadExact(PacketHeader.StructLength));
        var buffer = PoolBuffer.Rent(header.DataSize);
        try { await Stream.ReadExactlyAsync(buffer.Memory, Cancellation); return new(header, buffer); }
        catch { buffer.Free(); throw; }
    }

    public async Task SendInput(byte[] packet)
    {
        await writes.WaitAsync(Cancellation);
        try { await Stream.WriteAsync(packet, Cancellation); }
        finally { writes.Release(); }
    }
    public override Task Flush() => throw new IOException("The connection to the Switch ended.");
    public override Task StopStreaming() { Dispose(); return Task.CompletedTask; }
    public override void Dispose() { udp?.Dispose(); udp = null; udpQueue?.Writer.TryComplete(); client?.Dispose(); client = null; }
}
