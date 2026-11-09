using Microsoft.AspNetCore.Hosting.Server;
using Microsoft.AspNetCore.Hosting.Server.Features;
using NXRemotePlay.Protocol;
using System.Net;
using System.Security.Cryptography.X509Certificates;
using System.Text.Json;
using System.Text.Json.Serialization.Metadata;

namespace Rendezvous;

// Separate HTTPS control plane. Legacy UDP/TCP remains fenced for enrolled identities.
// There is deliberately no password-based enrollment or root-key HTTP endpoint.
public sealed class BridgeSignalingServer : IAsyncDisposable
{
    readonly HostOwnership ownership;
    readonly WebApplication app;
    readonly BridgeRtcBroker rtc;
    readonly int directPort;
    readonly P2pIceConfiguration ice;
    readonly object rateGate=new();
    readonly Dictionary<IPAddress,(long Start,int Count)> buckets=new();
    public Uri Address { get; private set; }=null!;
    public event Action<string>? Diagnostic;
    internal bool RegisterSessionLink(HostRoute route,string token) => rtc.RegisterSessionLink(route,token);
    internal string LocalHostTicket(HostRoute route) => rtc.HostTicket(route)
        ?? throw new InvalidOperationException("Browser session is no longer available.");

    public BridgeSignalingServer(HostOwnership ownership, IPAddress address, int port, X509Certificate2? certificate, bool loopbackBrowserTest=false,int directPort=7921,string[]? discoveryUrls=null,Func<X509Certificate2>? certificateProvider=null)
    {
        if(certificate is null && !IPAddress.IsLoopback(address))
            throw new ArgumentException("Public bridge signaling requires TLS.");
        if(loopbackBrowserTest&&!IPAddress.IsLoopback(address)) throw new ArgumentException("Browser fixture must stay on loopback.");
        this.ownership=ownership;
        if(directPort is <1 or >65535) throw new ArgumentException("Invalid direct signaling port.");
        this.directPort=directPort;
        ice=new P2pIceConfiguration(discoveryUrls ?? []);
        rtc=new(ownership,ice,directPort,message=>Diagnostic?.Invoke(message));
        var builder=WebApplication.CreateSlimBuilder();
        builder.Logging.ClearProviders(); // Request credentials/proofs never enter framework logs.
        builder.WebHost.ConfigureKestrel(server => {
            server.Limits.MaxRequestBodySize=4096;
            server.Limits.MaxConcurrentConnections=64;
            server.Limits.RequestHeadersTimeout=TimeSpan.FromSeconds(5);
            server.Limits.KeepAliveTimeout=TimeSpan.FromSeconds(15);
            server.Listen(address,port,listen => {
                if(certificate is not null) {
                    if(certificateProvider is null) listen.UseHttps(certificate);
                    else listen.UseHttps(options=>options.ServerCertificateSelector=(_,_)=>certificateProvider());
                }
            });
            if(loopbackBrowserTest&&certificate is not null) server.Listen(IPAddress.Loopback,0);
        });
        app=builder.Build();
        app.UseWebSockets(new WebSocketOptions {KeepAliveInterval=TimeSpan.FromSeconds(10)});
        app.Use(async (context,next) => {
            context.Response.Headers.CacheControl="no-store";
            if(!Allow(context.Connection.RemoteIpAddress??IPAddress.None)) { context.Response.StatusCode=429; return; }
            if(context.WebSockets.IsWebSocketRequest) { await next(context); return; }
            using var timeout=CancellationTokenSource.CreateLinkedTokenSource(context.RequestAborted);
            timeout.CancelAfter(TimeSpan.FromSeconds(5));
            context.RequestAborted=timeout.Token;
            try { await next(context); }
            catch(Exception ex) when(ex is ArgumentException or JsonException or BadHttpRequestException) {
                if(!context.Response.HasStarted) context.Response.StatusCode=400;
            }
            catch(OperationCanceledException) { if(!context.Response.HasStarted) context.Response.StatusCode=408; }
        });
        app.MapPost("/v1/owner/challenge",Challenge);
        app.MapPost("/v1/owner/proof",Proof);
        app.MapPost("/v1/join",Join);
        app.MapPost("/v1/rtc/session",rtc.Join);
        // HTTP/1.1 upgrades use GET; HTTPS browsers can negotiate HTTP/2,
        // whose WebSocket handshake uses extended CONNECT.
        app.MapMethods("/v1/rtc/socket",new[] {"GET","CONNECT"},rtc.Socket);
        foreach(var asset in new[] {("/","index.html","text/html; charset=utf-8"),
            ("/app.js","app.js","text/javascript; charset=utf-8"),("/controller.js","controller.js","text/javascript; charset=utf-8"),("/upscale.js","upscale.js","text/javascript; charset=utf-8"),("/fsr.js","fsr.js","text/javascript; charset=utf-8"),("/assets/LICENSE-FSR1.txt","assets/LICENSE-FSR1.txt","text/plain"),("/app.css","app.css","text/css; charset=utf-8"),
            ("/manifest.webmanifest","manifest.webmanifest","application/manifest+json"),
            ("/sw.js","sw.js","text/javascript; charset=utf-8"),
            ("/assets/icon-192.png","assets/icon-192.png","image/png"),("/assets/icon-512.png","assets/icon-512.png","image/png"),("/assets/icon-maskable-512.png","assets/icon-maskable-512.png","image/png"),("/assets/apple-touch-icon.png","assets/apple-touch-icon.png","image/png"),
            ("/assets/app_logo.png","assets/app_logo.png","image/png"),
            ("/assets/Baloo2-ExtraBold.ttf","assets/Baloo2-ExtraBold.ttf","font/ttf"),
            ("/assets/Baloo2-OFL.txt","assets/Baloo2-OFL.txt","text/plain")}) {
            app.MapGet(asset.Item1,async context=> {
                context.Response.ContentType=asset.Item3;
                context.Response.Headers.ContentSecurityPolicy="default-src 'self'; img-src 'self' blob:; media-src 'self' blob:; script-src 'self'; style-src 'self'; font-src 'self'; connect-src 'self'; frame-ancestors 'none'; object-src 'none'; base-uri 'none'";
                context.Response.Headers.XContentTypeOptions="nosniff";
                context.Response.Headers["Referrer-Policy"]="no-referrer";
                using var resource=typeof(BridgeSignalingServer).Assembly.GetManifestResourceStream("Rendezvous.Web."+asset.Item2.Replace('/','.'))
                    ??throw new InvalidOperationException("Missing browser asset.");
                await resource.CopyToAsync(context.Response.Body,context.RequestAborted);
            });
        }
    }
    bool Allow(IPAddress address)
    {
        lock(rateGate) {
            long now=Environment.TickCount64;
            foreach(var key in buckets.Keys.ToArray()) if(now-buckets[key].Start>=10000) buckets.Remove(key);
            if(!buckets.TryGetValue(address,out var bucket)) {
                if(buckets.Count>=4096) return false;
                bucket=(now,0);
            }
            buckets[address]=(bucket.Start,bucket.Count+1);
            return bucket.Count<60;
        }
    }
    static async Task<T> Read<T>(HttpContext context,JsonTypeInfo<T> info) =>
        await JsonSerializer.DeserializeAsync(context.Request.Body,info,context.RequestAborted)
            ?? throw new ArgumentException("Missing request.");
    static Task Write<T>(HttpContext context,T value,JsonTypeInfo<T> info) {
        context.Response.ContentType="application/json";
        return JsonSerializer.SerializeAsync(context.Response.Body,value,info,context.RequestAborted);
    }
    async Task Challenge(HttpContext context)
    {
        var request=await Read(context,BridgeJson.Default.BridgeChallengeRequest);
        if(request.Action>2 || request.Kind!=1) throw new ArgumentException("Invalid bridge owner operation.");
        _=NativeBridgeRoute.Parse(request.Endpoint);
        if(!ownership.IsProtected(request.Id)) { context.Response.StatusCode=404; return; }
        ulong generation=request.Generation;
        if(generation==0 && request.Action==0) generation=ownership.Generation(request.Id);
        var challenge=ownership.Challenge(request.Id,(OwnerAction)request.Action,(HostKind)request.Kind,
            request.Owner,request.Endpoint,generation);
        if(challenge is null) { context.Response.StatusCode=409; return; }
        var canonical=HostOwnership.Canonical(challenge);
        await Write(context,new BridgeChallengeReply(request with {Generation=generation},challenge.Nonce,
            challenge.ExpiresAt.ToUnixTimeMilliseconds(),canonical),BridgeJson.Default.BridgeChallengeReply);
    }
    async Task Proof(HttpContext context)
    {
        var supplied=await Read(context,BridgeJson.Default.BridgeProofRequest);
        var request=supplied.Request??throw new ArgumentException("Missing challenge.");
        if(request.Action>2||request.Kind!=1) throw new ArgumentException("Invalid bridge owner operation.");
        _=NativeBridgeRoute.Parse(request.Endpoint);
        var challenge=new OwnerChallenge(request.Id,(OwnerAction)request.Action,(HostKind)request.Kind,
            request.Owner,request.Endpoint,request.Generation,supplied.Nonce,DateTimeOffset.FromUnixTimeMilliseconds(supplied.ExpiresAt));
        var route=ownership.Authenticate(challenge,supplied.Proof);
        if(route is null) { context.Response.StatusCode=403; return; }
        await Write(context,new BridgeRouteReply(route.Id,(byte)route.Kind,route.Owner,route.Endpoint,
            route.Generation,route.ExpiresAt.ToUnixTimeMilliseconds(),rtc.HostTicket(route),IceServers(context),false,
            ownership.PunchTickets.Issue(route),directPort),BridgeJson.Default.BridgeRouteReply);
    }
    async Task Join(HttpContext context)
    {
        var request=await Read(context,BridgeJson.Default.BridgeJoinRequest);
        var route=ownership.Resolve(request.Id);
        if(route is null || !(route.Kind==HostKind.Direct ? DirectOwnerWire.Accepts(route,request.Password) :
            NativeBridgeRoute.Parse(route.Endpoint).Accepts(request.Id,request.Password))) {
            context.Response.StatusCode=404; return;
        }
        string endpoint=route.Kind==HostKind.Direct ? new DirectJoinRoute(context.Request.Host.Host,directPort).ToString() : route.Endpoint;
        await Write(context,new BridgeJoinReply(route.Id,(byte)route.Kind,endpoint,route.Generation,
            route.ExpiresAt.ToUnixTimeMilliseconds(),directPort),BridgeJson.Default.BridgeJoinReply);
    }
    internal RtcIceServer[] IceServers(HttpContext context)
    {
        return ice.For(context.Request.Host.Host,directPort);
    }
    public async Task Start(CancellationToken cancel=default)
    {
        await app.StartAsync(cancel);
        var addresses=app.Services.GetRequiredService<IServer>().Features.Get<IServerAddressesFeature>()!;
        Address=new Uri(addresses.Addresses.Single(value=>value.StartsWith(certificateScheme(),StringComparison.Ordinal)));
    }
    string certificateScheme()=>app.Urls.Any(value=>value.StartsWith("https:",StringComparison.Ordinal))?"https:":"http:";
    public async ValueTask DisposeAsync()
    {
        rtc.Stop();
        // Bounded: an open WebSocket must not hold the listener ; a cancelled stop closes the remaining connections.
        using var limit=new CancellationTokenSource(1500);
        try { await app.StopAsync(limit.Token); } catch(OperationCanceledException) { }
        await app.DisposeAsync();
    }
}
