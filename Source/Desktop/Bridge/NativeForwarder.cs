using NxRemotePlay.Client.Core;
using NxRemotePlay.Client.Sources;
using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Buffers.Binary;
using System.Diagnostics;

namespace NxRemotePlay.Client.Bridge;

// Native media adapter only. Does not register/discover itself or claim bridge ownership.
// The coordinator must provide an acquired, normalized USB source and an authorized route.
// Capacity: up to ViewerLimits.MaxRemoteViewers remote viewers (shared with browser viewers through the
// ViewerRegistry), each with its own queue and, on a slot-capable console, its own virtual pad. Local
// viewing/input stays independent. A slow viewer is disconnected without affecting the others.
internal sealed class NativeForwarder : IAsyncDisposable
{
    readonly ViewerRegistry registry;
    readonly bool ownsRegistry;
    readonly string password;
    readonly object gate = new();
    readonly CancellationTokenSource lifetime = new();
    readonly SemaphoreSlim pendingSlots = new(4);
    readonly HashSet<Task> handlers = new();
    readonly Dictionary<IPAddress, (long Window, int Count)> attempts = new();
    readonly TcpListener listener;
    readonly int capacity;
    readonly Func<bool>? authorized;
    Task? accepting;
    readonly List<Peer> viewers = new();
    bool stopped;
    internal IPEndPoint Endpoint => (IPEndPoint)listener.LocalEndpoint;
    // Native viewers connected right now.
    internal int ViewerCount { get { lock (gate) return viewers.Count; } }
    // False once the shared (native + browser) viewer limit is reached; discovery then answers BUSY.
    internal bool HasRoom => !registry.Full;

    sealed class Peer : IDisposable
    {
        internal readonly TcpClient Client;
        internal readonly CancellationTokenSource Cancel;
        internal readonly NativeMediaQueue Queue;
        internal readonly bool Video, Audio;
        internal readonly ViewerLease Lease;
        // This viewer's own pad (or the host's shared pad on an older console); null = view-only.
        internal BridgeInputArbiter? Controls => Lease.Controls;
        internal bool Input => Lease.Controls is not null;
        internal Peer(TcpClient client, bool video, bool audio, ViewerLease lease, int capacity, CancellationToken cancel)
        { Client = client; Video = video; Audio = audio; Lease = lease; Queue = new(capacity); Cancel = CancellationTokenSource.CreateLinkedTokenSource(cancel); }
        // This viewer's UDP video place (null = TCP video). Released with the peer.
        internal UdpmViewerPlace.Session? Udp;
        internal void Stop() { Cancel.Cancel(); Client.Dispose(); }
        public void Dispose() { Stop(); Queue.Dispose(); Udp?.Dispose(); Cancel.Dispose(); }
    }

    // `controls` is the hosting desktop's pad: it is the shared pad an older console gives its first viewer.
    // Pass `registry` (owned by the coordinator, shared with the browser host) for per-viewer pads.
    // Request byte 9 feature bits and the acknowledgement word, as the Android/NRO receivers send and expect them.
    const byte FeedbackFeature = 0x10, UdpmFeature = 0x20;
    const uint FeedbackAck = 0x3346514e;
    // 0x7e: the receiver's delivery-feedback report (32 bytes) interleaved with its input packets on the TCP stream.
    const byte FeedbackReportType = 0x7e;
    const int FeedbackReportSize = 32;
    readonly UdpmViewerPlace? udpm;

    internal NativeForwarder(string id, string password, IPAddress address, int port = 0, int queueCapacity = 96, Func<bool>? authorized = null, BridgeInputArbiter? controls = null, ViewerRegistry? registry = null, UdpmViewerPlace? udpm = null)
    {
        PunchSource.ValidateCredentials(id, password);
        this.udpm = udpm;
        this.password = password; capacity = queueCapacity;
        this.authorized = authorized;
        ownsRegistry = registry is null;
        this.registry = registry ?? new ViewerRegistry(null, controls);
        if (capacity is < 1 or > 128) throw new ArgumentOutOfRangeException(nameof(queueCapacity));
        listener = new TcpListener(address, port);
    }
    internal void Start()
    {
        lock (gate)
        {
            if (accepting is not null || stopped) throw new InvalidOperationException("Forwarder cannot be restarted.");
            listener.Start(4); accepting = Accept();
        }
    }
    bool Allow(IPAddress address)
    {
        lock (gate)
        {
            long now = Environment.TickCount64;
            foreach (var key in new List<IPAddress>(attempts.Keys))
                if (now - attempts[key].Window >= 10000) attempts.Remove(key);
            if (!attempts.TryGetValue(address, out var entry)) entry = (now, 0);
            if (entry.Count >= 10 || attempts.Count >= 1024 && !attempts.ContainsKey(address)) return false;
            attempts[address] = (entry.Window, entry.Count + 1); return true;
        }
    }
    async Task Accept()
    {
        try
        {
            while (!lifetime.IsCancellationRequested)
            {
                var client = await listener.AcceptTcpClientAsync(lifetime.Token).ConfigureAwait(false);
                var endpoint = (IPEndPoint)client.Client.RemoteEndPoint;
                if (!Allow(endpoint.Address) || !pendingSlots.Wait(0)) { client.Dispose(); continue; }
                var handling = Handle(client);
                lock (gate) handlers.Add(handling);
                _ = handling.ContinueWith(task => { lock (gate) handlers.Remove(task); }, CancellationToken.None,
                    TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default);
            }
        }
        catch (Exception) when (lifetime.IsCancellationRequested) { }
    }
    internal Task AcceptPunched(TcpClient client)
    {
        lock (gate)
        {
            if (stopped || !pendingSlots.Wait(0)) { client.Dispose(); return Task.CompletedTask; }
            var handling = Handle(client, punched: true); handlers.Add(handling);
            _ = handling.ContinueWith(task => { lock (gate) handlers.Remove(task); }, CancellationToken.None,
                TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default);
            return handling;
        }
    }
    static Task Line(Stream stream, string value, CancellationToken cancel) =>
        stream.WriteAsync(Encoding.ASCII.GetBytes(value + "\n"), cancel).AsTask();
    async Task Handle(TcpClient client, bool punched = false)
    {
        Peer? peer = null;
        ViewerLease? lease = null;
        UdpmViewerPlace.Session? udp = null;
        using var setup = CancellationTokenSource.CreateLinkedTokenSource(lifetime.Token);
        setup.CancelAfter(10000);
        try
        {
            if (authorized is not null && !authorized()) return;
            client.NoDelay = true;
            Stream stream = client.GetStream();
            if (!punched)
            {
                string nonce = Convert.ToHexString(RandomNumberGenerator.GetBytes(8)).ToLowerInvariant();
                await Line(stream, "NXLAN1 CHALLENGE " + nonce, setup.Token);
                var auth = (await PunchSource.ReadLine(stream, setup.Token)).Split(' ', StringSplitOptions.RemoveEmptyEntries);
                bool valid = auth.Length == 4 && auth[0] == "NXLAN1" && auth[1] == "AUTH" && auth[2].Length == 16 && auth[3].Length == 64;
                if (valid)
                {
                    foreach (char c in auth[2] + auth[3]) if (!char.IsAsciiHexDigit(c)) valid = false;
                }
                if (valid)
                {
                    var expected = SHA256.HashData(Encoding.ASCII.GetBytes(password + ":" + nonce + ":" + auth[2]));
                    valid = CryptographicOperations.FixedTimeEquals(expected, Convert.FromHexString(auth[3]));
                }
                if (!valid) { await Line(stream, "NXLAN1 ERR AUTH", setup.Token); return; }
                lease = registry.TryAcquire() ?? throw new IOException("Remote viewer limit reached.");
                await Line(stream, "NXLAN1 OK", setup.Token);
            }
            await stream.WriteAsync(Encoding.ASCII.GetBytes("NxRemotePlay|03\0"), setup.Token);
            var request = new byte[16]; await stream.ReadExactlyAsync(request, setup.Token);
            bool handshake = BinaryPrimitives.ReadUInt32LittleEndian(request) == 0xAAAAAAAA && request[4] == '0' && request[5] == '3' &&
                (request[6] & 3) != 0 && (request[6] & ~3) == 0 && (request[7] & ~7) == 0 &&
                request[8] <= StreamInfo.MaxAudioBatching;
            // byte 9 carries the Android feature bits (delivery feedback, UDP video). The desktop implements
            // none of them and does not acknowledge them (response bytes 4..7 stay 0), so the phone falls back to plain
            // TCP media; rejecting a non-zero byte made every current Android build fail its desktop join with code 2.
            for (int i = 10; i < 16; i++) if (request[i] != 0) handshake = false;
            var response = new byte[72];
            BinaryPrimitives.WriteUInt32LittleEndian(response, handshake ? 6u : 2u);
            if (!handshake) { await stream.WriteAsync(response, setup.Token); return; }
            lease ??= registry.TryAcquire() ?? throw new IOException("Remote viewer limit reached.");
            // Byte 9: delivery feedback (0x10) and UDP video (0x20), exactly as the console negotiates them. Feedback is
            // acknowledged in response bytes 4..7; the UDP offer (bit63 | port<<32 | token) follows in bytes 8..15.
            if ((request[9] & FeedbackFeature) != 0)
            {
                BinaryPrimitives.WriteUInt32LittleEndian(response.AsSpan(4), FeedbackAck);
                if ((request[9] & UdpmFeature) != 0 && udpm is not null && ((IPEndPoint)client.Client.RemoteEndPoint!).Address is var viewerAddress)
                {
                    udp = udpm.Offer(viewerAddress, out ulong offer);
                    if (udp is not null) BinaryPrimitives.WriteUInt64LittleEndian(response.AsSpan(8), offer);
                }
            }
            peer = new Peer(client, (request[6] & 1) != 0, (request[6] & 2) != 0, lease, capacity, lifetime.Token);
            peer.Udp = udp; udp = null; // owned by the peer from here on
            lock (gate)
            {
                if (stopped || authorized is not null && !authorized()) throw new IOException("Remote viewer unavailable.");
                viewers.Add(peer);
            }
            await stream.WriteAsync(response, setup.Token);
            var sending = Send(peer, stream);
            var receiving = Monitor(peer, stream);
            var watching = WatchInput(peer);
            await Task.WhenAny(sending, receiving, watching);
            peer.Stop();
            try { await Task.WhenAll(sending, receiving, watching); } catch (Exception) { }
        }
        catch (Exception) when (!System.Diagnostics.Debugger.IsAttached) { }
        finally
        {
            lock (gate) if (peer is not null) viewers.Remove(peer);
            peer?.Dispose(); udp?.Dispose(); client.Dispose();
            if (lease is not null) try { await lease.DisposeAsync(); } catch (Exception) { }
            pendingSlots.Release();
        }
    }
    async Task Monitor(Peer peer, Stream stream)
    {
        byte[] type = new byte[1];
        while (!peer.Cancel.IsCancellationRequested)
        {
            await stream.ReadExactlyAsync(type, peer.Cancel.Token);
            if (type[0] == FeedbackReportType)
            {
                // Delivery feedback (version 1, length 32): the desktop does not drive an encoder, so it is validated and dropped.
                byte[] report = new byte[FeedbackReportSize - 1];
                await stream.ReadExactlyAsync(report, peer.Cancel.Token);
                if (report[0] != 1 || report[1] != FeedbackReportSize) throw new IOException("Invalid native feedback report.");
                continue;
            }
            // A forwarder with no controller target at all rejects input; a viewer who simply has no pad
            // of its own (limit of an older console) is view-only and its input is read and ignored.
            if (!peer.Input && !registry.CanControl) throw new IOException("No controller target is attached.");
            int length = type[0] == 2 ? 17 : type[0] <= 3 ? 5 : throw new IOException("Invalid native input type.");
            byte[] packet = new byte[length]; packet[0] = type[0];
            await stream.ReadExactlyAsync(packet.AsMemory(1), peer.Cancel.Token);
            if (peer.Controls is not null && !await peer.Controls.NativeInput(packet)) throw new IOException("Invalid native input packet.");
        }
    }
    async Task WatchInput(Peer peer)
    {
        try
        {
            while (!peer.Cancel.IsCancellationRequested)
            {
                await Task.Delay(50, peer.Cancel.Token);
                if (authorized is not null && !authorized()) peer.Stop();
            }
        }
        finally { if (peer.Controls is not null) try { await peer.Controls.NativeDisconnected(); } catch (Exception) { } }
    }
    static async Task Send(Peer peer, Stream stream)
    {
        byte[] header = new byte[18];
        using var flowLog = OpenFlowLog();
        var sample = Stopwatch.StartNew();
        int maxPending = 0, packets = 0, videoPackets = 0, audioPackets = 0, screenshotPackets = 0;
        long payloadBytes = 0;
        double maxWriteMs = 0, maxInterSendMs = 0, maxVideoSendGapMs = 0;
        double maxVideoPublishGapMs = 0, maxVideoCaptureGapMs = 0, maxPublishToSendMs = 0;
        long previousSendStart = 0, previousVideoSendStart = 0, previousVideoPublish = 0;
        ulong previousVideoCapture = 0;
        await foreach (var entry in peer.Queue.ReadTimed(peer.Cancel.Token))
        {
            var packet = entry.Packet;
            try
            {
                MemoryMarshal.Write(header, in packet.Header);
                using var deadline = CancellationTokenSource.CreateLinkedTokenSource(peer.Cancel.Token);
                // A short gameplay burst can pause delivery for over two seconds;
                // keep the bounded queue alive through that transient gap.
                deadline.CancelAfter(5000);
                long writeStart = Stopwatch.GetTimestamp();
                if (previousSendStart != 0)
                    maxInterSendMs = Math.Max(maxInterSendMs, Stopwatch.GetElapsedTime(previousSendStart, writeStart).TotalMilliseconds);
                previousSendStart = writeStart;
                bool realVideo = packet.Header.IsVideo && !packet.Header.IsScreenshot && !packet.Header.IsReplay;
                if (realVideo)
                {
                    if (previousVideoSendStart != 0)
                        maxVideoSendGapMs = Math.Max(maxVideoSendGapMs, Stopwatch.GetElapsedTime(previousVideoSendStart, writeStart).TotalMilliseconds);
                    previousVideoSendStart = writeStart;
                    if (previousVideoPublish != 0)
                        maxVideoPublishGapMs = Math.Max(maxVideoPublishGapMs, Stopwatch.GetElapsedTime(previousVideoPublish, entry.PublishedAt).TotalMilliseconds);
                    previousVideoPublish = entry.PublishedAt;
                    if (previousVideoCapture != 0 && packet.Header.Timestamp >= previousVideoCapture)
                        maxVideoCaptureGapMs = Math.Max(maxVideoCaptureGapMs, (packet.Header.Timestamp - previousVideoCapture) / 1000.0);
                    previousVideoCapture = packet.Header.Timestamp;
                    maxPublishToSendMs = Math.Max(maxPublishToSendMs, Stopwatch.GetElapsedTime(entry.PublishedAt, writeStart).TotalMilliseconds);
                    videoPackets++;
                }
                else if (packet.Header.IsScreenshot) screenshotPackets++;
                else if (packet.Header.IsAudio) audioPackets++;
                // Video and audio leave together as UDPM frames once this viewer's UDP place is bound; everything else stays on TCP.
                if (!((packet.Header.IsVideo || packet.Header.IsAudio) && peer.Udp is { } udp && await SendMediaUdp(peer, udp, header, packet, deadline.Token)))
                {
                    await stream.WriteAsync(header, deadline.Token);
                    await stream.WriteAsync(packet.Buffer.Memory, deadline.Token);
                }
                double writeMs = Stopwatch.GetElapsedTime(writeStart).TotalMilliseconds;
                maxWriteMs = Math.Max(maxWriteMs, writeMs);
                maxPending = Math.Max(maxPending, peer.Queue.Pending);
                packets++;
                payloadBytes += packet.Header.DataSize;
                if (sample.ElapsedMilliseconds >= 1000)
                {
                    try
                    {
                        flowLog?.WriteLine($"{DateTime.UtcNow:O},{sample.ElapsedMilliseconds},{peer.Queue.Pending},{maxPending},{packets},{payloadBytes},{maxWriteMs:F2},{videoPackets},{audioPackets},{screenshotPackets},{maxInterSendMs:F2},{maxVideoSendGapMs:F2},{maxVideoPublishGapMs:F2},{maxVideoCaptureGapMs:F2},{maxPublishToSendMs:F2}");
                        flowLog?.Flush();
                    }
                    catch (IOException) { /* Diagnostics must never interrupt media. */ }
                    sample.Restart(); maxPending = packets = videoPackets = audioPackets = screenshotPackets = 0; payloadBytes = 0; maxWriteMs = maxInterSendMs = maxVideoSendGapMs = maxVideoPublishGapMs = maxVideoCaptureGapMs = maxPublishToSendMs = 0;
                }
            }
            finally { packet.Buffer?.Free(); }
        }
    }
    const uint PaceNormalBps = 20_000_000, PaceKeyframeBps = 45_000_000; // as the console: 2x the 10 Mbps target, keyframes faster
    // True when the packet went out over UDP; false = send it over TCP (UDP place never bound within its window).
    static async Task<bool> SendMediaUdp(Peer peer, UdpmViewerPlace.Session udp, byte[] header, ReceivedPacket packet, CancellationToken token)
    {
        if (!udp.IsBound && !await udp.WaitBound(token)) { peer.Udp = null; udp.Dispose(); return false; }
        if (!udp.Alive) throw new IOException("UDP media viewer went silent.");
        int size = header.Length + packet.Buffer.Memory.Length;
        byte[] wire = System.Buffers.ArrayPool<byte>.Shared.Rent(size);
        try
        {
            header.CopyTo(wire, 0); packet.Buffer.Memory.CopyTo(wire.AsMemory(header.Length));
            bool audio = packet.Header.IsAudio;
            bool key = !audio && (packet.Header.IsScreenshot || IsKeyframe(wire.AsSpan(0, size)));
            if (!audio) udp.Sender.SetPace(key ? PaceKeyframeBps : PaceNormalBps);
            long id = await udp.Sender.SendFrame(audio ? UdpmWire.TypeAudio : UdpmWire.TypeVideo, wire.AsMemory(0, size), (ulong)MonotonicClock.MicrosecondsNow,
                key ? UdpmWire.FlagKeyframe : (byte)0, token);
            if (id < 0) throw new IOException("UDP media send failed.");
        }
        finally { System.Buffers.ArrayPool<byte>.Shared.Return(wire); }
        return true;
    }
    // An IDR slice or SPS in the first 512 bytes of the payload (the hub injects SPS/PPS before IDRs), as UdpmVideo.c.
    static bool IsKeyframe(ReadOnlySpan<byte> wire)
    {
        if (wire.Length < 18) return false;
        var p = wire[18..]; int n = Math.Min(p.Length, 512);
        for (int i = 0; i + 3 < n; i++)
        {
            if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1)
            {
                int t = p[i + 3] & 0x1F;
                if (t == 5 || t == 7) return true;
                if (t == 1) return false;
            }
        }
        return false;
    }
    static StreamWriter? OpenFlowLog()
    {
        try
        {
            string folder = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "NX-Remote-Play", "Diagnostics");
            Directory.CreateDirectory(folder);
            string path = Path.Combine(folder, $"native-flow-{DateTime.UtcNow:yyyyMMdd-HHmmss}-{Guid.NewGuid():N}.csv");
            var writer = new StreamWriter(path);
            writer.WriteLine("utc,window_ms,pending_after_send,max_pending,packets,payload_bytes,max_write_ms,video_packets,audio_packets,screenshot_packets,max_inter_send_ms,max_video_send_gap_ms,max_video_publish_gap_ms,max_video_capture_gap_ms,max_publish_to_send_ms");
            return writer;
        }
        catch (Exception) { return null; }
    }
    // Borrowed buffer: called synchronously on the capture reader after replay expansion.
    internal void Publish(ReceivedPacket packet)
    {
        lock (gate)
        {
            if (authorized is not null && !authorized()) { foreach (var each in viewers) each.Stop(); return; }
            if (packet.Buffer is null) return;
            foreach (var peer in viewers)
            {
                if (packet.Header.IsVideo && !peer.Video || packet.Header.IsAudio && !peer.Audio) continue;
                if (!peer.Queue.TryPublish(packet)) peer.Stop();
            }
        }
    }
    internal void DisconnectViewer() { lock (gate) foreach (var peer in viewers) peer.Stop(); }
    public async ValueTask DisposeAsync()
    {
        Task? accept;
        lock (gate) { if (stopped) return; stopped = true; lifetime.Cancel(); listener.Stop(); foreach (var peer in viewers) peer.Stop(); accept = accepting; }
        if (accept is not null) await accept.ConfigureAwait(false);
        Task[] pending; lock (gate) pending = new List<Task>(handlers).ToArray();
        await Task.WhenAll(pending).ConfigureAwait(false);
        if (ownsRegistry) await registry.DisposeAsync();
        lifetime.Dispose(); pendingSlots.Dispose();
    }
}
