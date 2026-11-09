using NXRemotePlay.Protocol;
using System.Security.Cryptography;

namespace Rendezvous;

// Server half of first-use owner-key enrollment (see Shared/OwnerEnrollmentProtocol.cs for the wire
// format and the reasoning). Reached only through the "BRIDGEENROLL1" TCP command.
//
// Policy: the caller must prove it knows the live Session ID + password (a console that is
// registered with this server right now), and HostOwnership.EnrollFromPairing only accepts an ID
// that has no confirmed key. The key never reaches a log; only the ID and the outcome do.
internal static class OwnerEnrollmentService
{
    internal static async Task Handle(Stream stream, HostOwnership? ownership, Func<string, string?> sessionPassword,
        Action<string>? log, string remote, CancellationToken cancel)
    {
        async Task Reply(byte status, ReadOnlyMemory<byte> body = default)
        {
            var frame = new byte[1 + body.Length];
            frame[0] = status; body.CopyTo(frame.AsMemory(1));
            await OwnerEnrollmentProtocol.WriteFrame(stream, frame, cancel).ConfigureAwait(false);
        }

        var hello = await OwnerEnrollmentProtocol.ReadFrame(stream, cancel).ConfigureAwait(false);
        if (ownership is null) { await Reply(OwnerEnrollmentProtocol.Unavailable); return; }
        if (!OwnerEnrollmentProtocol.Server.TryReadId(hello, out string id)) { await Reply(OwnerEnrollmentProtocol.Malformed); return; }

        // Unknown id, a session without a pairing password (an already-protected host) and a wrong
        // password all look identical to the caller.
        string? password = sessionPassword(id);
        using var exchange = new OwnerEnrollmentProtocol.Server();
        var answer = string.IsNullOrEmpty(password) ? null : exchange.Accept(hello, password);
        if (answer is null)
        {
            log?.Invoke($"ENROLL from {remote} - refused for {id}");
            await Reply(OwnerEnrollmentProtocol.Refused);
            return;
        }
        await Reply(OwnerEnrollmentProtocol.Ok, answer);

        var finish = await OwnerEnrollmentProtocol.ReadFrame(stream, cancel).ConfigureAwait(false);
        var key = exchange.Open(finish);
        if (key is null) { log?.Invoke($"ENROLL from {remote} - bad proof for {id}"); await Reply(OwnerEnrollmentProtocol.Malformed); return; }
        try
        {
            // The session could have vanished (or its password changed) while the exchange ran.
            if (sessionPassword(id) != password) { await Reply(OwnerEnrollmentProtocol.Refused); return; }
            bool accepted = ownership.EnrollFromPairing(id, key);
            log?.Invoke(accepted ? $"ENROLL from {remote} - owner key stored for {id}" : $"ENROLL from {remote} - {id} is already locked to a different key");
            if (!accepted) { await Reply(OwnerEnrollmentProtocol.Locked); return; }
            await Reply(OwnerEnrollmentProtocol.Ok, OwnerEnrollmentProtocol.Fingerprint(key));
        }
        finally { CryptographicOperations.ZeroMemory(key); }
    }
}
