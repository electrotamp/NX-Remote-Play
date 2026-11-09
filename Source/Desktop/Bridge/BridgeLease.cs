using NXRemotePlay.Protocol;
using System;
using System.IO;
using System.Threading;
using System.Threading.Tasks;

namespace NxRemotePlay.Client.Bridge;

// Coordinator supplies the USB signer. Neither the client nor this lease reads the root key.
internal sealed class BridgeLease
{
    readonly IBridgeOwnerClient signal;
    readonly Func<byte[], CancellationToken, Task<byte[]>> sign;
    readonly BridgeChallengeRequest scope;
    readonly Func<byte, ulong, uint, CancellationToken, Task>? delegateUsb;
    ulong delegatedGeneration;
    readonly SemaphoreSlim operations = new(1);
    internal BridgeRouteReply? Route { get; private set; }
    internal BridgeLease(IBridgeOwnerClient signal, string id, string owner, string endpoint,
        Func<byte[], CancellationToken, Task<byte[]>> sign,
        Func<byte, ulong, uint, CancellationToken, Task>? delegateUsb = null)
    { this.signal = signal; this.sign = sign; this.delegateUsb = delegateUsb; scope = new(id, 0, 1, owner, endpoint, 0); }
    internal async Task<BridgeRouteReply> Execute(byte action, CancellationToken cancel)
    {
        await operations.WaitAsync(cancel);
        try { return await ExecuteInternal(action, cancel); } finally { operations.Release(); }
    }
    async Task<BridgeRouteReply> ExecuteInternal(byte action, CancellationToken cancel)
    {
        var request = scope with { Action = action, Generation = action == 0 ? 0 : Route?.Generation ?? throw new InvalidOperationException("No bridge lease.") };
        var challenge = await signal.Challenge(request, cancel);
        BridgeSignalingClient.ValidateChallenge(challenge, request);
        var proof = await sign(challenge.Canonical, cancel);
        bool prepared = false;
        try
        {
            if (action == 0)
            {
                // A recovered server route gets a new, attachment-scoped local
                // handoff. The old handoff is explicitly released first.
                Route = null;
                if (delegateUsb is not null && delegatedGeneration != 0)
                {
                    await delegateUsb(2, delegatedGeneration, 0, cancel);
                    delegatedGeneration = 0;
                }
            }
            if (action == 0 && delegateUsb is not null)
            {
                await delegateUsb(0, challenge.Request.Generation, 20000, cancel);
                delegatedGeneration = challenge.Request.Generation; prepared = true;
            }
            var route = await signal.Apply(new(challenge.Request, challenge.Nonce, challenge.ExpiresAt, proof), cancel);
            if (route.Id != scope.Id || route.Kind != scope.Kind || route.Owner != scope.Owner || route.Endpoint != scope.Endpoint ||
                action == 1 && route.Generation != request.Generation || action != 1 && route.Generation <= challenge.Request.Generation)
                throw new IOException("Signaling acknowledgement changed the requested owner scope.");
            if (action != 2)
            {
                long remaining = route.ExpiresAt - DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
                if (remaining <= 0 || remaining > 20000) throw new IOException("Invalid bridge lease lifetime.");
                // Commit one local USB handoff only after the server accepted a
                // claim. Server lease renewals retain fresh Switch signatures but
                // intentionally send no recurring USB delegation traffic.
                if (action == 0 && delegateUsb is not null)
                {
                    await delegateUsb(1, route.Generation, 20000, cancel);
                    delegatedGeneration = route.Generation;
                }
                Route = route;
            }
            else
            {
                Route = null;
                if (delegateUsb is not null && delegatedGeneration != 0)
                {
                    await delegateUsb(2, delegatedGeneration, 0, cancel); delegatedGeneration = 0;
                }
            }
            return route;
        }
        catch
        {
            // Before a server acknowledgement, abandon preparation. A lost HTTP reply may
            // leave a server lease, but direct recovery cannot displace it before expiry.
            if (prepared && delegateUsb is not null)
            {
                using var cleanup = new CancellationTokenSource(3000);
                try { await delegateUsb(2, delegatedGeneration, 0, cleanup.Token); delegatedGeneration = 0; } catch (Exception) { }
            }
            throw;
        }
        finally { System.Security.Cryptography.CryptographicOperations.ZeroMemory(proof); }
    }
}
