using NXRemotePlay.Protocol;
using System.Net.WebSockets;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Threading.Channels;

namespace Rendezvous;

// Only SDP/ICE pass through this broker. Media and controls use encrypted WebRTC.
// Tickets are one-use, short-lived and tied to the authoritative owner generation.
internal sealed class BridgeRtcBroker
{
    readonly HostOwnership ownership;
    readonly P2pIceConfiguration ice;
    readonly int directPort;
    readonly Action<string>? diagnostic;
    readonly object gate=new();
    readonly Dictionary<string,Ticket> tickets=new();
    // The session link : one token per browser session, reusable by every viewer until the session ends. Keyed by the SHA-256 of the
    // token so the secret itself is never kept; bound to the route's owner and generation, so it dies with the session.
    sealed record LinkToken(string Id, string Owner, ulong Generation);
    readonly Dictionary<string,LinkToken> linkTokens=new();
    static string LinkKey(string token)=>Convert.ToHexString(SHA256.HashData(Encoding.ASCII.GetBytes(token)));
    readonly Dictionary<string,Connection> hosts=new();
    readonly Dictionary<string,Connection> peers=new();
    sealed record Ticket(string Id, string Owner, ulong Generation, bool Host, DateTimeOffset ExpiresAt);
    sealed class Connection(WebSocket socket,Ticket scope,string session)
    {
        internal readonly WebSocket Socket=socket;
        internal readonly Ticket Scope=scope;
        internal readonly string Session=session;
        internal readonly CancellationTokenSource Cancel=new();
        internal readonly Channel<RtcEnvelope> Out=Channel.CreateBounded<RtcEnvelope>(new BoundedChannelOptions(32) {
            SingleReader=true,SingleWriter=false,FullMode=BoundedChannelFullMode.Wait,AllowSynchronousContinuations=false
        });
        internal void Stop() { Cancel.Cancel(); Socket.Abort(); Out.Writer.TryComplete(); }
        internal void Post(RtcEnvelope message) { if(!Out.Writer.TryWrite(message)) Stop(); }
    }
    internal BridgeRtcBroker(HostOwnership ownership,P2pIceConfiguration ice,int directPort=7921,Action<string>? diagnostic=null) {this.ownership=ownership;this.ice=ice;this.directPort=directPort;this.diagnostic=diagnostic;}
    internal void Stop()
    {
        lock(gate) {
            foreach(var connection in hosts.Values.Concat(peers.Values)) connection.Stop();
            tickets.Clear();
        }
    }
    void Prune()
    {
        var now=DateTimeOffset.UtcNow;
        foreach(var key in tickets.Keys.ToArray()) if(tickets[key].ExpiresAt<=now) tickets.Remove(key);
    }
    bool Current(Ticket ticket)
    {
        var route=ownership.Resolve(ticket.Id);
        return route?.Kind==HostKind.UsbBridge&&route.Owner==ticket.Owner&&route.Generation==ticket.Generation;
    }
    internal string? HostTicket(HostRoute route)
    {
        if(route.Kind!=HostKind.UsbBridge||route.ExpiresAt<=DateTimeOffset.UtcNow) return null;
        lock(gate) {
            Prune(); if(tickets.Count>=2048) return null;
            string token=Convert.ToHexString(RandomNumberGenerator.GetBytes(32));
            tickets.Add(token,new(route.Id,route.Owner,route.Generation,true,DateTimeOffset.UtcNow.AddSeconds(10)));
            return token;
        }
    }
    // Registers the one session link token (64 hex) for this session; replaces any earlier one. Joining with it never spends it.
    internal bool RegisterSessionLink(HostRoute route,string token)
    {
        if(route.Kind!=HostKind.UsbBridge||token.Length!=64) return false;
        lock(gate) { linkTokens.Clear(); linkTokens[LinkKey(token)]=new(route.Id,route.Owner,route.Generation); }
        return true;
    }
    internal Task Join(HttpContext context) => JoinImpl(context);
    async Task JoinImpl(HttpContext context)
    {
        var request=await JsonSerializer.DeserializeAsync(context.Request.Body,BridgeJson.Default.BridgeJoinRequest,context.RequestAborted);
        if(request is null) throw new ArgumentException("Missing join request.");
        var route=ownership.Resolve(request.Id);
        bool byLink=!string.IsNullOrEmpty(request.Token);
        if(route?.Kind!=HostKind.UsbBridge||(byLink?request.Token!.Length!=64:!NativeBridgeRoute.Parse(route.Endpoint).Accepts(request.Id,request.Password))) {
            context.Response.StatusCode=404; return;
        }
        RtcJoinReply? reply=null;
        bool linkRejected=false;
        lock(gate) {
            Prune();
            string? linkKey=null;
            if(byLink) {
                linkKey=LinkKey(request.Token!);
                if(!linkTokens.TryGetValue(linkKey,out var link)||link.Id!=route.Id||link.Owner!=route.Owner||link.Generation!=route.Generation) linkRejected=true;
            }
            if(!linkRejected) {
                // Viewers already admitted plus viewer tickets issued but not yet redeemed, so a burst of
                // simultaneous joins cannot exceed the limit before any socket connects.
                int viewers=peers.Values.Count(peer=>peer.Scope.Id==route.Id)+
                    tickets.Values.Count(ticket=>!ticket.Host&&ticket.Id==route.Id);
                if(hosts.TryGetValue(route.Id,out var host)&&Current(host.Scope)&&tickets.Count<2048&&
                    viewers<ViewerLimits.MaxRemoteViewers) {
                    string token=Convert.ToHexString(RandomNumberGenerator.GetBytes(32));
                    var expires=DateTimeOffset.UtcNow.AddSeconds(10);
                    tickets.Add(token,new(route.Id,route.Owner,route.Generation,false,expires));
                    reply=new(token,expires.ToUnixTimeMilliseconds(),ice.For(context.Request.Host.Host,directPort),false);
                }
            }
        }
        if(linkRejected) { context.Response.StatusCode=404; return; }
        if(reply is null) { context.Response.StatusCode=409; return; }
        context.Response.ContentType="application/json";
        await JsonSerializer.SerializeAsync(context.Response.Body,reply,BridgeJson.Default.RtcJoinReply,context.RequestAborted);
    }
    internal async Task Socket(HttpContext context)
    {
        if(!context.WebSockets.IsWebSocketRequest) { context.Response.StatusCode=400; return; }
        string token=context.Request.Query["ticket"].ToString();
        if(token.Length!=64) { context.Response.StatusCode=403; return; }
        Ticket? ticket;
        lock(gate) {
            Prune();
            if(!tickets.Remove(token,out ticket)||!Current(ticket)) ticket=null;
        }
        if(ticket is null) {diagnostic?.Invoke("RTC socket rejected: expired, consumed or stale ticket.");context.Response.StatusCode=403;return;}
        // Browser joins must originate on this HTTPS page; host clients supply no Origin.
        string origin=context.Request.Headers.Origin.ToString();
        if((!ticket.Host||origin.Length!=0)&&origin!=$"{context.Request.Scheme}://{context.Request.Host}") {
            diagnostic?.Invoke("RTC socket rejected: page origin mismatch.");
            context.Response.StatusCode=403; return;
        }
        using var socket=await context.WebSockets.AcceptWebSocketAsync();
        var connection=new Connection(socket,ticket,ticket.Host?"":token);
        bool admitted=false;
        lock(gate) {
            if(Current(ticket)) {
                if(ticket.Host) {
                    if(hosts.TryGetValue(ticket.Id,out var old)) old.Stop();
                    foreach(var peer in peers.Values.Where(peer=>peer.Scope.Id==ticket.Id).ToArray()) peer.Stop();
                    hosts[ticket.Id]=connection; admitted=true;
                } else if(hosts.TryGetValue(ticket.Id,out var host)&&Current(host.Scope)&&
                    peers.Values.Count(peer=>peer.Scope.Id==ticket.Id)<ViewerLimits.MaxRemoteViewers) {
                    peers[token]=connection; admitted=true;
                }
            }
        }
        if(!admitted) { connection.Stop(); connection.Cancel.Dispose(); return; }
        diagnostic?.Invoke(ticket.Host?"RTC host socket admitted.":"RTC viewer socket admitted.");
        connection.Post(new(connection.Session,"ready",""));
        var sending=Send(connection);
        var watching=Watch(connection);
        try {
            while(!connection.Cancel.IsCancellationRequested) {
                var envelope=await Receive(connection);
                if(envelope is null) break;
                if(envelope.Type=="offer") diagnostic?.Invoke("RTC browser offer received.");
                if(envelope.Type is not ("offer" or "answer" or "candidate" or "close") ||
                    envelope.Payload is null||envelope.Payload.Length>(envelope.Type is "offer" or "answer"?32768:2048))
                    break;
                lock(gate) {
                    if(!Current(ticket)) break;
                    if(ticket.Host) {
                        if(envelope.Type=="offer") break;
                        if(peers.TryGetValue(envelope.Session,out var peer)&&peer.Scope.Id==ticket.Id&&
                            peer.Scope.Generation==ticket.Generation&&peer.Scope.Owner==ticket.Owner)
                            peer.Post(envelope);
                    } else {
                        if(envelope.Type=="answer") break;
                        if(hosts.TryGetValue(ticket.Id,out var host)&&Current(host.Scope))
                            host.Post(envelope with {Session=connection.Session});
                        else break;
                    }
                }
            }
        } catch(Exception ex) when(ex is WebSocketException or OperationCanceledException or JsonException or DecoderFallbackException) { }
        finally {
            diagnostic?.Invoke(ticket.Host?"RTC host socket ended.":"RTC viewer socket ended.");
            connection.Stop();
            try { await Task.WhenAll(sending,watching); } catch(Exception ex) when(ex is WebSocketException or OperationCanceledException) { }
            lock(gate) {
                if(ticket.Host) {
                    if(hosts.TryGetValue(ticket.Id,out var host)&&ReferenceEquals(host,connection)) {
                        hosts.Remove(ticket.Id);
                        foreach(var peer in peers.Values.Where(peer=>peer.Scope.Id==ticket.Id).ToArray()) peer.Stop();
                    }
                } else {
                    peers.Remove(connection.Session);
                    if(hosts.TryGetValue(ticket.Id,out var host)&&Current(host.Scope)) host.Post(new(connection.Session,"close",""));
                }
            }
            connection.Cancel.Dispose();
        }
    }
    async Task Watch(Connection connection)
    {
        while(!connection.Cancel.IsCancellationRequested) {
            await Task.Delay(1000,connection.Cancel.Token);
            if(!Current(connection.Scope)) { connection.Stop(); break; }
        }
    }
    static async Task Send(Connection connection)
    {
        try {
        await foreach(var message in connection.Out.Reader.ReadAllAsync(connection.Cancel.Token)) {
            var bytes=JsonSerializer.SerializeToUtf8Bytes(message,BridgeJson.Default.RtcEnvelope);
            using var deadline=CancellationTokenSource.CreateLinkedTokenSource(connection.Cancel.Token);
            deadline.CancelAfter(3000);
            await connection.Socket.SendAsync(bytes.AsMemory(),WebSocketMessageType.Text,true,deadline.Token);
        }
        } finally { connection.Stop(); }
    }
    static async Task<RtcEnvelope?> Receive(Connection connection)
    {
        byte[] bytes=new byte[40960]; int used=0;
        while(used<bytes.Length) {
            var result=await connection.Socket.ReceiveAsync(bytes.AsMemory(used),connection.Cancel.Token);
            if(result.MessageType==WebSocketMessageType.Close) return null;
            if(result.MessageType!=WebSocketMessageType.Text) return null;
            used+=result.Count;
            if(result.EndOfMessage) return JsonSerializer.Deserialize(bytes.AsSpan(0,used),BridgeJson.Default.RtcEnvelope);
        }
        return null;
    }
}
