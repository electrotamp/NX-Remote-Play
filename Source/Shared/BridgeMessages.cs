using System;
using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Serialization;

namespace NXRemotePlay.Protocol;

public sealed record BridgeChallengeRequest(string Id, byte Action, byte Kind, string Owner, string Endpoint, ulong Generation);
public sealed record BridgeChallengeReply(BridgeChallengeRequest Request, string Nonce, long ExpiresAt, byte[] Canonical);
public sealed record BridgeProofRequest(BridgeChallengeRequest Request, string Nonce, long ExpiresAt, byte[] Proof);
public sealed record RtcIceServer(string[] Urls, string? Username = null, string? Credential = null);
public sealed record BridgeRouteReply(string Id, byte Kind, string Owner, string Endpoint, ulong Generation, long ExpiresAt, string? SocketToken = null, RtcIceServer[]? IceServers = null, bool RelayOnly = false, string? PunchTicket = null, int PunchPort = 0);
// Token: the session link token of the desktop's browser server ; Password is then ignored. Older clients omit it.
public sealed record BridgeJoinRequest(string Id, string Password, string? Token = null);
public sealed record BridgeJoinReply(string Id, byte Kind, string Endpoint, ulong Generation, long ExpiresAt, int PunchPort = 0);
public sealed record RtcJoinReply(string Ticket, long ExpiresAt, RtcIceServer[]? IceServers = null, bool RelayOnly = false);
public sealed record RtcEnvelope(string Session, string Type, string Payload);
public sealed record NativeOwnerRequest(string Operation, BridgeChallengeRequest? Challenge = null, BridgeProofRequest? Proof = null);
public sealed record NativeOwnerReply(BridgeChallengeReply? Challenge = null, BridgeRouteReply? Route = null, string? Error = null, byte[]? Authentication = null);
// Reuse the console's scoped owner signer to verify server replies. The reply
// digest is domain-separated and never a live server-issued ownership nonce.
public static class NativeOwnerAuthentication
{
    public static byte[] Canonical(BridgeChallengeRequest scope, ulong generation, long expires, NativeOwnerReply reply)
    {
        var payload = System.Text.Json.JsonSerializer.SerializeToUtf8Bytes(reply with { Authentication = null }, BridgeJson.Default.NativeOwnerReply);
        var domain = Encoding.ASCII.GetBytes("NXRP-NATIVE-OWNER-REPLY-1:");
        var bytes = new byte[domain.Length + payload.Length]; domain.CopyTo(bytes, 0); payload.CopyTo(bytes, domain.Length);
        string nonce = Convert.ToHexString(SHA256.HashData(bytes));
        return OwnerProofBytes.Serialize(scope.Id, scope.Action, scope.Kind, scope.Owner, scope.Endpoint, generation, nonce, expires);
    }
}
public sealed record DirectJoinRoute(string Host, int Port)
{
    public override string ToString() => "punch/" + Host + "/" + Port;
    public static DirectJoinRoute Parse(string text)
    {
        var parts = text.Split('/');
        if (parts.Length != 3 || parts[0] != "punch" || parts[1].Length is < 1 or > 253 ||
            Uri.CheckHostName(parts[1]) == UriHostNameType.Unknown || !int.TryParse(parts[2], out int port) || port is < 1 or > 65535)
            throw new ArgumentException("Invalid direct host route.");
        return new(parts[1], port);
    }
}

[JsonSerializable(typeof(BridgeChallengeRequest))]
[JsonSerializable(typeof(BridgeChallengeReply))]
[JsonSerializable(typeof(BridgeProofRequest))]
[JsonSerializable(typeof(BridgeRouteReply))]
[JsonSerializable(typeof(BridgeJoinRequest))]
[JsonSerializable(typeof(BridgeJoinReply))]
[JsonSerializable(typeof(RtcJoinReply))]
[JsonSerializable(typeof(RtcEnvelope))]
[JsonSerializable(typeof(RtcIceServer[]))]
[JsonSerializable(typeof(NativeOwnerRequest))]
[JsonSerializable(typeof(NativeOwnerReply))]
[JsonSourceGenerationOptions(PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase)]
public partial class BridgeJson : JsonSerializerContext { }

// Signed desktop endpoint metadata. Production native clients use the original
// Switch protocol; historical TLS diagnostic metadata remains readable.
public sealed record NativeBridgeRoute(IPEndPoint Address, string CertificateHash, string PairingHash)
{
    public NativeBridgeRoute(IPEndPoint address, string pairingHash) : this(address, new string('0', 64), pairingHash) { }
    public override string ToString() => CertificateHash == new string('0', 64) ? $"native/{Address}/{PairingHash}" : $"tls/{Address}/{CertificateHash}/{PairingHash}";
    public static NativeBridgeRoute Parse(string text)
    {
        if (text is null) throw new ArgumentException("Missing bridge endpoint.");
        var fields = text.Split('/');
        if (fields.Length == 3 && fields[0] == "native") fields = ["tls", fields[1], new string('0', 64), fields[2]];
        if (fields.Length != 4 || fields[0] != "tls" || !IPEndPoint.TryParse(fields[1], out var endpoint) ||
            endpoint.Port == 0 || endpoint.Address.Equals(IPAddress.Any) || endpoint.Address.Equals(IPAddress.IPv6Any) ||
            fields[2].Length != 64 || fields[3].Length != 64)
            throw new ArgumentException("Invalid bridge endpoint.");
        foreach (char c in fields[2] + fields[3]) if (!char.IsAsciiHexDigit(c)) throw new ArgumentException("Invalid bridge verifier.");
        return new(endpoint, fields[2].ToUpperInvariant(), fields[3].ToUpperInvariant());
    }
    public static string HashPassword(string id, string password)
    {
        if (id is null || password is null || id.Length != 5 || password.Length != 5) throw new ArgumentException("Invalid pairing credentials.");
        foreach (char c in id + password) if (!char.IsAsciiLetterOrDigit(c)) throw new ArgumentException("Invalid pairing credentials.");
        return Convert.ToHexString(SHA256.HashData(Encoding.ASCII.GetBytes("NXRP-JOIN-1:" + id + ":" + password)));
    }
    public bool Accepts(string id, string password) => CryptographicOperations.FixedTimeEquals(
        Convert.FromHexString(PairingHash), Convert.FromHexString(HashPassword(id, password)));
}
