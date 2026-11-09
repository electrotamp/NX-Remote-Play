using NXRemotePlay.Protocol;
using System;
using System.Buffers.Binary;
using System.IO;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
namespace NxRemotePlay.Client.Bridge;

internal sealed class NativeOwnerClient : IBridgeOwnerClient
{
    readonly Uri address;
    readonly Func<byte[], CancellationToken, Task<byte[]>> sign;
    public string ServerHost => address.Host;
    internal static Uri ParseAddress(string value)
    {
        if (!Uri.TryCreate("tcp://" + value.Trim(), UriKind.Absolute, out var uri) || uri.Port is < 1 or > 65535 || uri.AbsolutePath != "/" || uri.UserInfo.Length != 0 || uri.Query.Length != 0 || uri.Fragment.Length != 0 || Uri.CheckHostName(uri.Host) == UriHostNameType.Unknown)
            throw new ArgumentException("Enter the rendezvous host and native port, for example host:7921.");
        return uri;
    }
    internal NativeOwnerClient(string address, Func<byte[], CancellationToken, Task<byte[]>> sign) { this.address = ParseAddress(address); this.sign = sign; }
    async Task<NativeOwnerReply> Send(NativeOwnerRequest request, CancellationToken cancel)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancel); timeout.CancelAfter(5000);
        using var client = new TcpClient { NoDelay = true }; await client.ConnectAsync(address.Host, address.Port, timeout.Token);
        var stream = client.GetStream(); await stream.WriteAsync(Encoding.ASCII.GetBytes("BRIDGEOWNER1\n"), timeout.Token);
        var bytes = JsonSerializer.SerializeToUtf8Bytes(request, BridgeJson.Default.NativeOwnerRequest);
        if (bytes.Length is < 1 or > 4096) throw new IOException("Owner request too large.");
        var header = new byte[4]; BinaryPrimitives.WriteInt32BigEndian(header, bytes.Length);
        await stream.WriteAsync(header, timeout.Token); await stream.WriteAsync(bytes, timeout.Token);
        await stream.ReadExactlyAsync(header, timeout.Token); int size = BinaryPrimitives.ReadInt32BigEndian(header);
        if (size is < 1 or > 4096) throw new IOException("Rendezvous needs the updated native Bridge Mode server.");
        bytes = new byte[size]; await stream.ReadExactlyAsync(bytes, timeout.Token);
        var reply = JsonSerializer.Deserialize(bytes, BridgeJson.Default.NativeOwnerReply) ?? throw new IOException("Empty owner response.");
        if (reply.Error is not null) throw new IOException(reply.Error switch
        {
            "OWNERSHIP_NOT_CONFIGURED" => "Rendezvous ownership registry is not configured.",
            "IDENTITY_NOT_ENROLLED" => "Switch identity is not enrolled on this rendezvous server.",
            "OWNER_CONFLICT" => "Rendezvous ownership changed; reconnecting.",
            _ => "Rendezvous rejected bridge ownership."
        });
        return reply;
    }
    async Task Verify(NativeOwnerReply reply, BridgeChallengeRequest scope, ulong generation, long expires, CancellationToken cancel)
    {
        if (reply.Authentication?.Length != 32) throw new IOException("Unauthenticated rendezvous reply.");
        byte[] proof = await sign(NativeOwnerAuthentication.Canonical(scope, generation, expires, reply), cancel);
        try { if (!CryptographicOperations.FixedTimeEquals(proof, reply.Authentication)) throw new IOException("Rendezvous reply authentication failed."); }
        finally { CryptographicOperations.ZeroMemory(proof); }
    }
    public async Task<BridgeChallengeReply> Challenge(BridgeChallengeRequest request, CancellationToken cancel)
    {
        var reply = await Send(new("challenge", Challenge: request), cancel);
        var challenge = reply.Challenge ?? throw new IOException("Missing owner challenge.");
        if (reply.Route is not null) throw new IOException("Unexpected owner route.");
        BridgeSignalingClient.ValidateChallenge(challenge, request);
        await Verify(reply, challenge.Request, challenge.Request.Generation, challenge.ExpiresAt, cancel); return challenge;
    }
    public async Task<BridgeRouteReply> Apply(BridgeProofRequest request, CancellationToken cancel)
    {
        var reply = await Send(new("proof", Proof: request), cancel);
        var route = reply.Route ?? throw new IOException("Missing owner route.");
        if (reply.Challenge is not null) throw new IOException("Unexpected owner challenge.");
        await Verify(reply, request.Request, request.Request.Generation, request.ExpiresAt, cancel); return route;
    }
    public void Dispose() { }
}
