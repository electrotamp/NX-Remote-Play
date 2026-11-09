using NXRemotePlay.Protocol;
using System;
using System.IO;
using System.Net;
using System.Net.Http;
using System.Net.Http.Json;
using System.Net.WebSockets;
using System.Security.Cryptography;
using System.Text.Json.Serialization.Metadata;
using System.Threading;
using System.Threading.Tasks;

namespace NxRemotePlay.Client.Bridge;

internal sealed class BridgeSignalingClient : IBridgeOwnerClient
{
    readonly HttpClient http;
    readonly Uri address;
    readonly byte[]? pin;
    public string ServerHost => address.Host;
    internal BridgeSignalingClient(Uri address, string? certificatePin = null)
    {
        if (address.Scheme != "https" || !string.IsNullOrEmpty(address.UserInfo) || address.AbsolutePath != "/")
            throw new ArgumentException("Bridge signaling requires an HTTPS server address.");
        this.address = address;
        var handler = new HttpClientHandler { AllowAutoRedirect = false };
        if (certificatePin is not null)
        {
            pin = Convert.FromHexString(certificatePin);
            if (pin.Length != 32) throw new ArgumentException("Invalid signaling certificate pin.");
            handler.ServerCertificateCustomValidationCallback = (_, cert, _, _) => cert is not null &&
                CryptographicOperations.FixedTimeEquals(cert.GetCertHash(HashAlgorithmName.SHA256), pin);
        }
        http = new HttpClient(handler) { BaseAddress = address, Timeout = TimeSpan.FromSeconds(5), MaxResponseContentBufferSize = 4096 };
    }
    internal async Task<ClientWebSocket> HostSocket(string ticket, CancellationToken cancel, bool viewer = false)
    {
        if (ticket is null || ticket.Length != 64) throw new ArgumentException("Invalid signaling ticket.");
        foreach (char c in ticket) if (!char.IsAsciiHexDigit(c)) throw new ArgumentException("Invalid signaling ticket.");
        var socket = new ClientWebSocket();
        if (viewer) socket.Options.SetRequestHeader("Origin", address.GetLeftPart(UriPartial.Authority));
        if (pin is not null) socket.Options.RemoteCertificateValidationCallback = (_, cert, _, _) => cert is not null &&
            CryptographicOperations.FixedTimeEquals(cert.GetCertHash(HashAlgorithmName.SHA256), pin);
        var destination = new UriBuilder(address) { Scheme = "wss", Path = "/v1/rtc/socket", Query = "ticket=" + ticket };
        try { await socket.ConnectAsync(destination.Uri, cancel); return socket; }
        catch { socket.Dispose(); throw; }
    }
    async Task<TResult> Post<T, TResult>(string path, T body, JsonTypeInfo<T> input, JsonTypeInfo<TResult> output, CancellationToken cancel)
    {
        using var response = await http.PostAsJsonAsync(path, body, input, cancel);
        response.EnsureSuccessStatusCode();
        // Read bounded response before JSON; do not accept unbounded/chunked JSON from a server.
        await response.Content.LoadIntoBufferAsync(4096, cancel);
        return await response.Content.ReadFromJsonAsync(output, cancel) ?? throw new IOException("Empty signaling response.");
    }
    public Task<BridgeChallengeReply> Challenge(BridgeChallengeRequest request, CancellationToken cancel) =>
        Post("v1/owner/challenge", request, BridgeJson.Default.BridgeChallengeRequest, BridgeJson.Default.BridgeChallengeReply, cancel);
    public Task<BridgeRouteReply> Apply(BridgeProofRequest request, CancellationToken cancel) =>
        Post("v1/owner/proof", request, BridgeJson.Default.BridgeProofRequest, BridgeJson.Default.BridgeRouteReply, cancel);
    internal Task<BridgeJoinReply> Join(string id, string password, CancellationToken cancel) =>
        Post("v1/join", new BridgeJoinRequest(id, password), BridgeJson.Default.BridgeJoinRequest, BridgeJson.Default.BridgeJoinReply, cancel);
    internal static void ValidateChallenge(BridgeChallengeReply reply, BridgeChallengeRequest requested)
    {
        var actual = reply.Request;
        if (actual is null || actual.Id != requested.Id || actual.Action != requested.Action || actual.Kind != requested.Kind ||
            actual.Owner != requested.Owner || actual.Endpoint != requested.Endpoint || actual.Generation == 0 ||
            requested.Generation != 0 && actual.Generation != requested.Generation)
            throw new IOException("Signaling challenge changed the requested owner scope.");
        long remaining = reply.ExpiresAt - DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        if (remaining <= 0 || remaining > 15000) throw new IOException("Signaling challenge expired or has invalid lifetime.");
        var expected = OwnerProofBytes.Serialize(actual.Id, actual.Action, actual.Kind, actual.Owner, actual.Endpoint, actual.Generation, reply.Nonce, reply.ExpiresAt);
        if (reply.Canonical is null || !expected.AsSpan().SequenceEqual(reply.Canonical))
            throw new IOException("Signaling challenge bytes do not match their scope.");
    }
    public void Dispose() => http.Dispose();
}
