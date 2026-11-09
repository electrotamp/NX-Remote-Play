using NXRemotePlay.Protocol;
using System;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace NxRemotePlay.Client.Bridge;

// What the desktop needs from a USB-attached console to install its owner key.
internal interface IOwnerEnrollmentTarget
{
    string SessionId { get; }
    string SessionPassword { get; }
    bool CanEnroll { get; }
    Task<byte[]> StageOwnerKey(ReadOnlyMemory<byte> key, CancellationToken cancel);
    Task<byte[]> CommitOwnerKey(CancellationToken cancel);
}

// First-use setup : a console with no owner key gets one the first time it is plugged into
// any computer running the desktop app. Order matters: stage on the console -> enroll with the
// rendezvous server -> commit on the console. A failure at any step leaves the console's ordinary
// hosting untouched, and a retry simply starts again with a fresh key. The key lives only in this
// method's memory; it is never written to disk or logged.
internal static class OwnerKeyEnrollment
{
    internal static async Task Run(IOwnerEnrollmentTarget target, string server, CancellationToken cancel, Action<string>? status = null)
    {
        if (!target.CanEnroll) return;
        string id = target.SessionId, password = target.SessionPassword;
        if (id.Length != 5 || string.IsNullOrEmpty(password)) throw new IOException("The Switch has no Session ID yet; try again in a moment.");
        var address = NativeOwnerClient.ParseAddress(server);
        var key = RandomNumberGenerator.GetBytes(32);
        try
        {
            var fingerprint = OwnerEnrollmentProtocol.Fingerprint(key);
            status?.Invoke("USB Viewer: setting up bridge ownership (first time)");
            Compare(await target.StageOwnerKey(key, cancel).ConfigureAwait(false), fingerprint, "The Switch stored a different key than the one sent.");
            Compare(await EnrollWithServer(address, id, password, key, cancel).ConfigureAwait(false), fingerprint, "The rendezvous server stored a different key than the one sent.");
            Compare(await target.CommitOwnerKey(cancel).ConfigureAwait(false), fingerprint, "The Switch committed a different key than the one sent.");
        }
        finally { CryptographicOperations.ZeroMemory(key); }
    }

    static void Compare(byte[] actual, byte[] expected, string message)
    {
        if (!CryptographicOperations.FixedTimeEquals(actual, expected)) throw new IOException(message);
    }

    internal static async Task<byte[]> EnrollWithServer(Uri address, string id, string password, byte[] key, CancellationToken cancel)
    {
        // The console registers with the server on its own whenever it has a network. When it has none
        // (the case Bridge Mode exists for) the server has not seen this code yet, so register the same
        // code on the console's behalf once and retry; this uses the ordinary pairing path and cannot
        // replace a code someone else holds.
        for (int attempt = 0; ; attempt++)
        {
            var (code, body) = await Exchange(address, id, password, key, cancel).ConfigureAwait(false);
            if (code == OwnerEnrollmentProtocol.Ok) return body;
            if (code == OwnerEnrollmentProtocol.Refused && attempt == 0 && await RegisterCode(address, id, password, cancel).ConfigureAwait(false)) continue;
            throw new IOException(OwnerEnrollmentProtocol.Describe(code));
        }
    }

    static async Task<(byte Code, byte[] Body)> Exchange(Uri address, string id, string password, byte[] key, CancellationToken cancel)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancel);
        timeout.CancelAfter(8000);
        using var client = new TcpClient { NoDelay = true };
        await client.ConnectAsync(address.Host, address.Port, timeout.Token).ConfigureAwait(false);
        var stream = client.GetStream();
        await stream.WriteAsync(Encoding.ASCII.GetBytes(OwnerEnrollmentProtocol.Command + "\n"), timeout.Token).ConfigureAwait(false);
        using var exchange = new OwnerEnrollmentProtocol.Client(id, password);
        await OwnerEnrollmentProtocol.WriteFrame(stream, exchange.Hello(), timeout.Token).ConfigureAwait(false);
        byte[] reply;
        try { reply = await OwnerEnrollmentProtocol.ReadFrame(stream, timeout.Token).ConfigureAwait(false); }
        catch (Exception error) when (error is EndOfStreamException or IOException)
        { throw new IOException("Rendezvous needs the updated server to set up bridge ownership."); }
        if (reply[0] != OwnerEnrollmentProtocol.Ok) return (reply[0], Array.Empty<byte>());
        await OwnerEnrollmentProtocol.WriteFrame(stream, exchange.Finish(reply.AsSpan(1), key), timeout.Token).ConfigureAwait(false);
        var result = await OwnerEnrollmentProtocol.ReadFrame(stream, timeout.Token).ConfigureAwait(false);
        return (result[0], result[1..]);
    }

    static async Task<bool> RegisterCode(Uri address, string id, string password, CancellationToken cancel)
    {
        var addresses = await Dns.GetHostAddressesAsync(address.Host, AddressFamily.InterNetwork, cancel).ConfigureAwait(false);
        if (addresses.Length == 0) return false;
        using var udp = new UdpClient(AddressFamily.InterNetwork);
        udp.Connect(addresses[0], address.Port);
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancel);
        timeout.CancelAfter(3000);
        await udp.SendAsync(Encoding.ASCII.GetBytes($"REGISTER {id} {password}"), timeout.Token).ConfigureAwait(false);
        var reply = Encoding.ASCII.GetString((await udp.ReceiveAsync(timeout.Token).ConfigureAwait(false)).Buffer);
        return reply == $"REGISTERED {id} {password}";
    }
}
