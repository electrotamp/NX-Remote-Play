using NXRemotePlay.Protocol;
using NxRemotePlay.Client.Core;
using NxRemotePlay.Client.Sources;
using System;
using System.IO;
using System.Threading;
using System.Threading.Tasks;

namespace NxRemotePlay.Client.Bridge;
internal static class BridgeJoinConnector
{
    internal static async Task<PunchSource> Connect(Uri server, string id, string password, StreamingOptions options,
        CancellationToken cancel, string? pin = null, Action<string>? message = null, bool discoverLan = true)
    {
        using var signaling = new BridgeSignalingClient(server, pin);
        // Preserve the native console's server-independent LAN Join first.
        if (discoverLan) using (var lan = new PunchSource(server.Host, 7921, id, password, options, cancel))
            {
                var endpoint = await lan.Discover();
                if (endpoint is not null)
                {
                    var local = new PunchSource(server.Host, 7921, id, password, options, cancel);
                    try { await local.ConnectDirect(endpoint); return local; } catch { local.Dispose(); throw; }
                }
            }
        var route = await signaling.Join(id, password, cancel);
        long remaining = route.ExpiresAt - DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        if (route.Id != id || route.Generation == 0 || route.Kind > 1 || remaining <= 0 || remaining > 20000)
            throw new IOException("The host route is expired or invalid.");
        var direct = route.Kind == 0 ? DirectJoinRoute.Parse(route.Endpoint) : null;
        var source = new PunchSource(direct?.Host ?? server.Host, direct?.Port ?? 7921, id, password, options, cancel);
        if (message is not null) source.OnMessage += message;
        try
        {
            // HTTPS selects the rendezvous address only. Media and controls use the
            // same native PUNCHJOIN and NxRemotePlay protocol as every existing client.
            if (route.Kind == 1)
            {
                source.Dispose();
                source = new PunchSource(server.Host, route.PunchPort > 0 ? route.PunchPort : 7921, id, password, options, cancel);
                if (message is not null) source.OnMessage += message;
            }
            await source.ConnectNative(discoverLan: false);
            return source;
        }
        catch { source.Dispose(); throw; }
        finally { if (message is not null) source.OnMessage -= message; }
    }
}
