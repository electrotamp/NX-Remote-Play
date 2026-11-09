using NXRemotePlay.Protocol;
using NxRemotePlay.Client.Sources;
using SIPSorcery.Net;
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net.WebSockets;
using System.Text.Json;
using System.Threading;
using System.Threading.Channels;
using System.Threading.Tasks;

namespace NxRemotePlay.Client.Bridge;

// One signaling socket, any number of browser viewers (up to the shared viewer limit, with native viewers
// counted by the same ViewerRegistry). Each offer is its own session and gets its own peer connection, media
// queue and, on a slot-capable console, its own virtual pad; a peer that fails or leaves never disturbs the others.
internal sealed class BrowserRtcHost : IAsyncDisposable
{
    sealed class Viewer(BrowserRtcPeer peer, ViewerLease lease)
    {
        internal readonly BrowserRtcPeer Peer = peer;
        internal readonly ViewerLease Lease = lease;
        readonly long created = Environment.TickCount64;
        // Ended, or never finished connecting: a viewer must not hold a place indefinitely.
        internal bool Dead => Peer.Ended || Peer.Connection.connectionState != RTCPeerConnectionState.connected && Environment.TickCount64 - created > 30000;
    }
    readonly ClientWebSocket socket;
    readonly CancellationTokenSource cancel;
    readonly Channel<RtcEnvelope> outgoing = Channel.CreateBounded<RtcEnvelope>(32);
    readonly RTCConfiguration configuration;
    readonly ViewerRegistry registry;
    readonly bool ownsRegistry;
    readonly int mediaPort;
    readonly Func<System.Net.IPEndPoint?>? mappedCandidate;
    readonly object gate = new();
    readonly Dictionary<string, Viewer> viewers = new();
    readonly Task reading, writing, controlling;
    internal bool IsActive => !reading.IsCompleted && !writing.IsCompleted && !cancel.IsCancellationRequested;
    internal bool HasParticipant { get { lock (gate) return viewers.Values.Any(viewer => viewer.Peer.Connection.connectionState == RTCPeerConnectionState.connected); } }
    internal int ParticipantCount { get { lock (gate) return viewers.Values.Count(viewer => viewer.Peer.Connection.connectionState == RTCPeerConnectionState.connected); } }
    internal BrowserRtcHost(ClientWebSocket socket, RTCConfiguration configuration, CancellationToken lifetime, BridgeInputArbiter? controls = null, bool automaticControl = false, int mediaPort = 0, Func<System.Net.IPEndPoint?>? mappedCandidate = null, ViewerRegistry? registry = null)
    {
        this.socket = socket; this.configuration = configuration; cancel = CancellationTokenSource.CreateLinkedTokenSource(lifetime);
        ownsRegistry = registry is null;
        this.registry = registry ?? new ViewerRegistry(null, controls);
        this.mediaPort = mediaPort; this.mappedCandidate = mappedCandidate;
        reading = Task.Run(Read); writing = Task.Run(Write);
        controlling = Task.Run(() => Maintain(automaticControl));
    }
    // Frees the place of any viewer whose connection ended without a signaling "close" (an ICE failure), and,
    // when automatic control is on, hands each connected viewer its pad.
    async Task Maintain(bool automaticControl)
    {
        try
        {
            while (!cancel.IsCancellationRequested)
            {
                Viewer[] current; lock (gate) current = viewers.Values.ToArray();
                foreach (var dead in current.Where(viewer => viewer.Dead))
                {
                    bool removed; lock (gate) removed = viewers.Remove(dead.Peer.Session, out _);
                    if (removed) await End(dead);
                }
                if (automaticControl) foreach (var viewer in current)
                    {
                        if (viewer.Dead) continue;
                        // Each viewer with a pad is granted it as soon as its input channel opens. A viewer sharing the
                        // host's pad (older console) is granted only while no other remote holds it.
                        if (viewer.Lease.Controls is not { } pad || viewer.Peer.Connection.connectionState != RTCPeerConnectionState.connected || viewer.Peer.HasControl || !viewer.Peer.MayGrantAutomatically) continue;
                        if (viewer.Lease.Slot == 0 && pad.RemoteHasControl) continue;
                        try { await viewer.Peer.GrantControl(); }
                        catch (InvalidOperationException) { } // Input channel is still opening or the viewer left.
                    }
                await Task.Delay(100, cancel.Token);
            }
        }
        catch (OperationCanceledException) when (cancel.IsCancellationRequested) { }
        catch (Exception) { cancel.Cancel(); }
    }
    void Post(RtcEnvelope message) { if (!outgoing.Writer.TryWrite(message)) cancel.Cancel(); }
    internal void Publish(ReceivedPacket packet) { lock (gate) foreach (var viewer in viewers.Values) viewer.Peer.Publish(packet); }
    async Task Read()
    {
        byte[] bytes = new byte[40960];
        try
        {
            while (!cancel.IsCancellationRequested)
            {
                int count = 0; ValueWebSocketReceiveResult result;
                do
                {
                    if (count == bytes.Length) throw new IOException("Signaling message exceeds limit.");
                    result = await socket.ReceiveAsync(bytes.AsMemory(count), cancel.Token);
                    if (result.MessageType == WebSocketMessageType.Close) return;
                    if (result.MessageType != WebSocketMessageType.Text) throw new IOException("Invalid signaling message.");
                    count += result.Count;
                } while (!result.EndOfMessage);
                var message = JsonSerializer.Deserialize(bytes.AsSpan(0, count), BridgeJson.Default.RtcEnvelope) ?? throw new IOException("Empty signaling message.");
                if (message.Type == "ready") continue;
                if (message.Type == "offer")
                {
                    if (message.Session.Length != 64 || message.Payload.Length > 32768) throw new IOException("Invalid browser offer.");
                    // A repeated offer for one session replaces only that session's viewer.
                    await ClearPeer(message.Session);
                    var lease = registry.TryAcquire();
                    if (lease is null) { Post(new(message.Session, "close", "")); continue; }
                    // The fixed (router-mapped) media port can be bound once; later viewers use automatic ports.
                    bool first; lock (gate) first = viewers.Count == 0;
                    BrowserRtcPeer next;
                    try { next = new BrowserRtcPeer(configuration, cancel.Token, lease.Controls, message.Session, first ? mediaPort : 0); }
                    catch { await lease.DisposeAsync(); throw; }
                    lock (gate) viewers[message.Session] = new Viewer(next, lease);
                    string current = message.Session;
                    RtcFlowLog.Event(current, "offer", $"viewers={ParticipantCount} fixedMediaPort={(first ? mediaPort : 0)}");
                    next.Connection.onicecandidate += candidate =>
                    {
                        if (candidate is not null)
                        {
                            RtcFlowLog.Event(current, "local_candidate", $"{candidate.type} {candidate.protocol} {candidate.address}:{candidate.port}");
                            Post(new(current, "candidate", candidate.toJSON()));
                        }
                    };
                    next.Connection.onconnectionstatechange += state => RtcFlowLog.Event(current, "connection_state", state.ToString());
                    next.Connection.oniceconnectionstatechange += state => RtcFlowLog.Event(current, "ice_state", state.ToString());
                    next.Connection.onicegatheringstatechange += state => RtcFlowLog.Event(current, "ice_gathering", state.ToString());
                    var accepted = next.Connection.setRemoteDescription(new RTCSessionDescriptionInit { type = RTCSdpType.offer, sdp = message.Payload });
                    if (accepted != SetDescriptionResultEnum.OK)
                        throw new IOException("Browser offer was not accepted (" + accepted + ").");
                    System.Net.IPEndPoint? publicEndpoint = mappedCandidate?.Invoke();
                    if (publicEndpoint is { } mapped)
                    {
                        RtcFlowLog.Event(current, "mapped_candidate", mapped.ToString());
                        next.Connection.addLocalIceCandidate(new RTCIceCandidate(RTCIceProtocol.udp, mapped.Address, (ushort)mapped.Port, RTCIceCandidateType.host));
                    }
                    else RtcFlowLog.Event(current, "mapped_candidate", "none (no active router mapping for the media port)");
                    var answer = next.Connection.createAnswer();
                    await next.Connection.setLocalDescription(answer);
                    string answerSdp = next.Connection.localDescription.sdp.ToString();
                    answerSdp = AddStreamIds(answerSdp);
                    Post(new(current, "answer", answerSdp));
                    RtcFlowLog.Event(current, "answer_sent", $"candidateLines={System.Linq.Enumerable.Count(answerSdp.Split('\n'), line => line.StartsWith("a=candidate"))}");
                    // addLocalIceCandidate never raises onicecandidate, so the router-mapped public address was never sent to the
                    // browser: a browser on another network had nothing it could reach (, rtc-flow log). Send it explicitly,
                    // after the answer (the browser can only apply remote candidates once it holds the answer).
                    if (publicEndpoint is { } publicAddress && publicAddress.AddressFamily == System.Net.Sockets.AddressFamily.InterNetwork)
                    {
                        Post(new(current, "candidate", MappedCandidateJson(publicAddress)));
                        RtcFlowLog.Event(current, "mapped_candidate_sent", publicAddress.ToString());
                    }
                }
                else
                {
                    BrowserRtcPeer? peer; lock (gate) peer = viewers.TryGetValue(message.Session, out var viewer) ? viewer.Peer : null;
                    if (peer is null) continue;
                    if (message.Type == "close") await ClearPeer(message.Session);
                    else if (message.Type == "candidate")
                    {
                        using var candidate = JsonDocument.Parse(message.Payload);
                        var root = candidate.RootElement;
                        RtcFlowLog.Event(message.Session, "remote_candidate", root.TryGetProperty("candidate", out var text) && text.ValueKind == JsonValueKind.String ? text.GetString() ?? "" : "");
                        peer.Connection.addIceCandidate(new RTCIceCandidateInit
                        {
                            candidate = root.GetProperty("candidate").GetString(),
                            sdpMid = root.TryGetProperty("sdpMid", out var mid) && mid.ValueKind == JsonValueKind.String ? mid.GetString() : "0",
                            sdpMLineIndex = root.TryGetProperty("sdpMLineIndex", out var index) && index.ValueKind == JsonValueKind.Number ? (ushort)index.GetInt32() : (ushort)0
                        });
                    }
                }
            }
        }
        finally { cancel.Cancel(); socket.Abort(); await ClearAll(); }
    }
    // The audio and the video m-section get different stream ids (a=msid) so the browser does not lip-sync them
    // (browsers tie audio and video together when both belong to the same stream, and delay the video to match the audio buffer; measured with the local browser harness).
    internal static string AddStreamIds(string sdp)
    {
        var lines = sdp.Replace("\r\n", "\n").Split('\n');
        var result = new System.Text.StringBuilder();
        string? kind = null;
        void Close()
        {
            if (kind == "audio") result.Append("a=msid:nxaudio nxaudio0\r\n");
            else if (kind == "video") result.Append("a=msid:nxvideo nxvideo0\r\n");
        }
        foreach (var line in lines)
        {
            if (line.Length == 0) continue;
            if (line.StartsWith("m=")) { Close(); kind = line.StartsWith("m=audio") ? "audio" : line.StartsWith("m=video") ? "video" : null; }
            if (line.StartsWith("a=msid:")) continue;   // never duplicate one the stack already wrote
            result.Append(line).Append("\r\n");
        }
        Close();
        return result.ToString();
    }
    // Same shape the browser itself sends for a server-reflexive candidate. sdpMid "0" always exists in the browser's offer
    // (a candidate for a missing m-line makes addIceCandidate throw), and every media section shares this one transport.
    internal static string MappedCandidateJson(System.Net.IPEndPoint endpoint) =>
        $"{{\"candidate\":\"candidate:9 1 udp 1677729535 {endpoint.Address} {endpoint.Port} typ srflx raddr 0.0.0.0 rport 0\",\"sdpMid\":\"0\",\"sdpMLineIndex\":0}}";
    async Task ClearPeer(string session)
    {
        Viewer? old; lock (gate) { viewers.Remove(session, out old); }
        if (old is not null) await End(old);
    }
    async Task ClearAll()
    {
        Viewer[] old; lock (gate) { old = viewers.Values.ToArray(); viewers.Clear(); }
        foreach (var viewer in old) await End(viewer);
    }
    static async Task End(Viewer viewer)
    {
        try { await viewer.Peer.DisposeAsync(); }
        finally { await viewer.Lease.DisposeAsync(); }
    }
    async Task Write()
    {
        try
        {
            await foreach (var message in outgoing.Reader.ReadAllAsync(cancel.Token))
            {
                byte[] bytes = JsonSerializer.SerializeToUtf8Bytes(message, BridgeJson.Default.RtcEnvelope);
                using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancel.Token); timeout.CancelAfter(3000);
                await socket.SendAsync(bytes.AsMemory(), WebSocketMessageType.Text, true, timeout.Token);
            }
        }
        finally { cancel.Cancel(); socket.Abort(); }
    }
    public async ValueTask DisposeAsync()
    {
        cancel.Cancel(); socket.Abort();
        try { await Task.WhenAll(reading, writing, controlling); }
        catch (Exception ex) when (ex is WebSocketException or OperationCanceledException) { }
        finally
        {
            await ClearAll();
            if (ownsRegistry) await registry.DisposeAsync();
            socket.Dispose(); cancel.Dispose();
        }
    }
}
