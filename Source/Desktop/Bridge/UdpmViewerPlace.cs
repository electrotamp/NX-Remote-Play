using System;
using System.Buffers.Binary;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Threading;
using System.Threading.Tasks;

namespace NxRemotePlay.Client.Bridge;

// The desktop bridge's UDPM video place, the same behaviour as the console's UdpmVideo.c: ONE native viewer at a time may
// carry its video over UDP. Negotiation rides on the existing TCP handshake (a viewer that negotiated delivery feedback
// and sets the UDPM feature bit receives, in the response's bytes 8..15, bit63 | port<<32 | session token). The viewer
// then sends UDPM Hello datagrams from its UDP socket; once one arrives from the TCP peer's IP with the right token,
// every video wire packet (18-byte header + payload) leaves as one UDPM frame. Audio, input and control stay on TCP.
// No Hello within the bind window -> the viewer simply keeps TCP video. Not implemented here (the Switch has them): the
// UDP rendezvous/NAT key capability and receiver-driven rate control (the encoder is on the Switch, behind USB).
internal sealed class UdpmViewerPlace : IAsyncDisposable
{
    internal const ulong OfferBit = 1UL << 63;
    internal const int DefaultPort = 9951;
    internal const long BindWindowMs = 3000, SilentLimitMs = 5000;
    readonly Socket socket;
    readonly CancellationTokenSource cancel = new();
    readonly Task receiving;
    readonly object gate = new();
    readonly StreamWriter? flowLog;
    Session? current;
    long nextLogTicks;
    int disposed;
    // Router-visible port for viewers outside the LAN (set once the NAT-PMP UDP mapping exists); 0 = none.
    internal volatile int ExternalPort;
    internal int Port => ((IPEndPoint)socket.LocalEndPoint!).Port;

    internal sealed class Session : IDisposable
    {
        readonly UdpmViewerPlace owner;
        internal readonly uint Token;
        internal readonly IPAddress PeerAddress;
        internal readonly long OfferedAt = Environment.TickCount64;
        internal readonly UdpmSender Sender;
        readonly TaskCompletionSource<bool> bound = new(TaskCreationOptions.RunContinuationsAsynchronously);
        volatile IPEndPoint? remote;
        long lastHeard;
        internal uint Nacks, KeyframeRequests, Hellos, FeedbackReports;
        internal UdpmWire.Feedback Last;
        internal Session(UdpmViewerPlace owner, uint token, IPAddress peer)
        {
            this.owner = owner; Token = token; PeerAddress = peer; lastHeard = Environment.TickCount64;
            Sender = new UdpmSender(new UdpmSender.Config(), token, 128, Send);
        }
        bool Send(ReadOnlyMemory<byte> datagram)
        {
            var to = remote; if (to is null) return false;
            try { owner.socket.SendTo(datagram.Span, SocketFlags.None, to); return true; }
            catch (SocketException ex) when (ex.SocketErrorCode is SocketError.WouldBlock or SocketError.NoBufferSpaceAvailable) { return true; } // counted as loss; FEC/NACK cover it
            catch (Exception ex) when (ex is SocketException or ObjectDisposedException) { return false; }
        }
        internal bool IsBound => remote is not null;
        // Silent for SilentLimitMs after binding (or never bound within the window) = the viewer is gone.
        internal bool Alive => IsBound ? Environment.TickCount64 - Interlocked.Read(ref lastHeard) < SilentLimitMs : true;
        internal void Heard() => Interlocked.Exchange(ref lastHeard, Environment.TickCount64);
        internal bool Bind(IPEndPoint from)
        {
            bool first = remote is null; remote = from; Heard();
            if (first) bound.TrySetResult(true);
            return first;
        }
        internal void Rebind(IPEndPoint from) { remote = from; Heard(); }
        // True once the viewer has bound; false when the bind window expired (the viewer keeps TCP video).
        internal async Task<bool> WaitBound(CancellationToken token)
        {
            if (bound.Task.IsCompleted) return true;
            long left = BindWindowMs - (Environment.TickCount64 - OfferedAt);
            if (left <= 0) return false;
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token); timeout.CancelAfter((int)left);
            try { return await bound.Task.WaitAsync(timeout.Token).ConfigureAwait(false); }
            catch (OperationCanceledException) when (!token.IsCancellationRequested) { return false; }
        }
        public void Dispose() { owner.Release(this); }
    }

    // port 0 = ephemeral (tests). Returns null when the port cannot be bound (the session then stays TCP-only).
    internal static UdpmViewerPlace? TryOpen(int port, Action<string>? log = null)
    {
        try { return new UdpmViewerPlace(port); }
        catch (SocketException ex) { log?.Invoke($"UDP video unavailable: port {port} ({ex.SocketErrorCode})"); return null; }
    }
    UdpmViewerPlace(int port)
    {
        socket = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
        try
        {
            socket.ReceiveBufferSize = 1 << 20; socket.SendBufferSize = 1 << 20;
            if (OperatingSystem.IsWindows()) socket.IOControl(-1744830452, new byte[] { 0, 0, 0, 0 }, null); // SIO_UDP_CONNRESET off: an ICMP unreachable must not end the loop
            socket.Bind(new IPEndPoint(IPAddress.Any, port));
        }
        catch { socket.Dispose(); throw; }
        flowLog = OpenFlowLog();
        TimerResolution.Begin();
        receiving = Task.Run(Receive);
    }
    static StreamWriter? OpenFlowLog()
    {
        try
        {
            string folder = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "NX-Remote-Play", "Diagnostics");
            Directory.CreateDirectory(folder);
            var writer = new StreamWriter(Path.Combine(folder, $"udpm-flow-{DateTime.UtcNow:yyyyMMdd-HHmmss}-{Guid.NewGuid():N}.csv"));
            writer.WriteLine("utc,frames,datagrams,parity,retransmits,send_errors,max_send_us,paced_ms,nacks,keyframe_requests,hellos,feedback,rx_packets,rx_lost,rx_fec,rx_frames_dropped,rx_jitter_us,rx_playout_us");
            return writer;
        }
        catch (Exception) { return null; }
    }

    // Reserves the single UDP place for a TCP peer. Returns null when it is taken (the viewer then uses TCP).
    internal Session? Offer(IPAddress tcpPeer, out ulong field)
    {
        field = 0;
        lock (gate)
        {
            if (current is not null || disposed != 0) return null;
            uint token; do { token = BinaryPrimitives.ReadUInt32LittleEndian(RandomNumberGenerator.GetBytes(4)); } while (token == 0);
            var session = new Session(this, token, tcpPeer); current = session;
            int port = IsPrivate(tcpPeer) || ExternalPort == 0 ? Port : ExternalPort;
            field = OfferBit | ((ulong)(ushort)port << 32) | token;
            return session;
        }
    }
    // LAN viewers use the real port; viewers outside the LAN use the router's external port.
    internal static bool IsPrivate(IPAddress a)
    {
        if (IPAddress.IsLoopback(a)) return true;
        if (a.IsIPv4MappedToIPv6) a = a.MapToIPv4();
        var b = a.GetAddressBytes();
        return a.AddressFamily == AddressFamily.InterNetwork && (b[0] == 10 || b[0] == 172 && b[1] is >= 16 and <= 31 || b[0] == 192 && b[1] == 168 || b[0] == 169 && b[1] == 254 || b[0] == 100 && b[1] is >= 64 and <= 127);
    }
    void Release(Session session)
    {
        lock (gate) if (ReferenceEquals(current, session)) current = null;
    }

    async Task Receive()
    {
        var buffer = new byte[2048];
        EndPoint any = new IPEndPoint(IPAddress.Any, 0);
        try
        {
            while (!cancel.IsCancellationRequested)
            {
                SocketReceiveFromResult result;
                try { result = await socket.ReceiveFromAsync(buffer, SocketFlags.None, any, cancel.Token).ConfigureAwait(false); }
                catch (SocketException) { continue; }
                Session? s; lock (gate) s = current;
                if (s is not null) Handle(s, buffer.AsSpan(0, result.ReceivedBytes), (IPEndPoint)result.RemoteEndPoint);
                LogTick();
            }
        }
        catch (Exception ex) when (ex is OperationCanceledException or ObjectDisposedException) { }
    }
    void Handle(Session s, ReadOnlySpan<byte> d, IPEndPoint from)
    {
        int type = UdpmWire.PeekType(d);
        if (type == UdpmWire.TypeHello && UdpmWire.TryReadHello(d, s.Token, out _))
        {
            if (!s.IsBound) { if (!from.Address.Equals(s.PeerAddress)) return; s.Bind(from); } // the token alone is not enough
            else if (from.Address.Equals(s.PeerAddress)) s.Rebind(from); // NAT may move the viewer's port
            else return;
            s.Hellos++; return;
        }
        if (!s.IsBound || !from.Address.Equals(s.PeerAddress)) return;
        if (type == UdpmWire.TypeNack && UdpmWire.TryReadNack(d, s.Token, out var nack)) { s.Sender.HandleNack(nack); s.Nacks++; }
        else if (type == UdpmWire.TypeKeyframeRequest && UdpmWire.TryReadKeyframeRequest(d, s.Token, out _)) s.KeyframeRequests++; // the encoder is on the Switch: next IDR serves it
        else if (type == UdpmWire.TypeFeedback && UdpmWire.TryReadFeedback(d, s.Token, out var fb)) { s.Last = fb; s.FeedbackReports++; }
        else return;
        s.Heard();
    }
    // Keeps the per-second CSV (and wakes up idle receive loops through ReceiveFrom timeouts is unnecessary: datagrams arrive each second).
    void LogTick()
    {
        long now = Environment.TickCount64;
        if (now < nextLogTicks) return;
        nextLogTicks = now + 1000;
        Session? s; lock (gate) s = current;
        if (s is null || flowLog is null) return;
        var st = s.Sender.TakeStats(); var f = s.Last;
        try
        {
            flowLog.WriteLine($"{DateTime.UtcNow:O},{st.Frames},{st.Datagrams},{st.ParityDatagrams},{st.Retransmits},{st.SendErrors},{st.MaxSendUs},{st.PacedWaitUs / 1000},{s.Nacks},{s.KeyframeRequests},{s.Hellos},{s.FeedbackReports},{f.PacketsReceived},{f.PacketsLost},{f.ShardsRecoveredFec},{f.FramesDropped},{f.JitterUs},{f.PlayoutDelayUs}");
            flowLog.Flush();
        }
        catch (IOException) { /* Diagnostics must never interrupt media. */ }
    }
    public async ValueTask DisposeAsync()
    {
        if (Interlocked.Exchange(ref disposed, 1) != 0) return;
        cancel.Cancel();
        try { socket.Dispose(); } catch (Exception) { }
        try { await receiving.ConfigureAwait(false); } catch (Exception) { }
        flowLog?.Dispose(); TimerResolution.End(); cancel.Dispose();
    }
}

// Task.Delay(1) is ~15 ms on Windows unless the system timer is raised; UDPM pacing needs ~1 ms steps while a place is open.
internal static class TimerResolution
{
    [DllImport("winmm.dll", EntryPoint = "timeBeginPeriod")] static extern uint BeginPeriod(uint ms);
    [DllImport("winmm.dll", EntryPoint = "timeEndPeriod")] static extern uint EndPeriod(uint ms);
    static int users;
    internal static void Begin() { if (OperatingSystem.IsWindows() && Interlocked.Increment(ref users) == 1) try { BeginPeriod(1); } catch (Exception) { } }
    internal static void End() { if (OperatingSystem.IsWindows() && Interlocked.Decrement(ref users) == 0) try { EndPeriod(1); } catch (Exception) { } }
}
