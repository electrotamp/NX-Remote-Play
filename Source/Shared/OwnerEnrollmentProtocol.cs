using System;
using System.Buffers.Binary;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace NXRemotePlay.Protocol;

// First-use owner-key enrollment. Linked into the rendezvous server and the desktop.
//
// A desktop that has a console attached over USB asks the rendezvous server to remember a fresh
// 256-bit owner key for that console's Session ID. The server accepts only while the ID has no
// confirmed key, and only for a caller that proves it knows the live Session ID + password. The key
// is encrypted to an ephemeral ECDH key so an eavesdropper on the path cannot read it; it is never
// derived from the ID or password and it never reaches a log.
//
// TCP, after the line "BRIDGEENROLL1\n". Every frame is [u16 big-endian length][body].
//   C->S hello : id(5) | u16 len | client SPKI | client nonce(16) | HMAC_pw("C1"|id|cpub|cnonce)
//   S->C reply : status(1) | [ u16 len | server SPKI | server nonce(16) | HMAC_pw("S1"|id|cpub|cnonce|spub|snonce) ]
//   C->S finish: GCM nonce(12) | ciphertext(32) | tag(16) | HMAC_pw("C2"|...|nonce|ciphertext|tag)
//   S->C result: status(1) | [ SHA-256(key)(32) ]
// The server verifies the client's password proof BEFORE it sends anything keyed by the password, so an
// off-path caller gets no offline password-guessing oracle and online guesses are rate limited.
// An observer who captures the client hello can still test guesses against its password-derived HMAC
// offline. The short pairing password makes this a known unresolved security limitation.
public static class OwnerEnrollmentProtocol
{
    public const string Command = "BRIDGEENROLL1";
    public const byte Ok = 0, Refused = 1, Locked = 2, Malformed = 3, Busy = 4, Unavailable = 5;
    public const int NonceLength = 16, KeyLength = 32, MaximumPublicKey = 120, MaximumFrame = 512;

    public static string Describe(byte status) => status switch
    {
        Ok => "enrolled",
        Refused => "The rendezvous server does not have this console registered right now.",
        Locked => "This Session ID already has an owner key. Refresh the code on the console to start fresh.",
        Malformed => "The rendezvous server rejected the enrollment request.",
        Busy => "Too many enrollment attempts; try again in a minute.",
        Unavailable => "This rendezvous server is not set up for bridge ownership.",
        _ => "The rendezvous server refused enrollment."
    };

    internal static byte[] Mac(string password, string label, params ReadOnlyMemory<byte>[] parts)
    {
        using var hmac = IncrementalHash.CreateHMAC(HashAlgorithmName.SHA256, Encoding.UTF8.GetBytes(password));
        hmac.AppendData(Encoding.ASCII.GetBytes("NXRP-ENROLL-" + label));
        Span<byte> length = stackalloc byte[2];
        foreach (var part in parts)
        {
            BinaryPrimitives.WriteUInt16BigEndian(length, checked((ushort)part.Length));
            hmac.AppendData(length);
            hmac.AppendData(part.Span);
        }
        return hmac.GetHashAndReset();
    }

    internal static byte[] DeriveKey(ReadOnlySpan<byte> shared, byte[] clientNonce, byte[] serverNonce, string id)
    {
        var salt = new byte[clientNonce.Length + serverNonce.Length];
        clientNonce.CopyTo(salt, 0); serverNonce.CopyTo(salt, clientNonce.Length);
        var output = new byte[32];
        HKDF.DeriveKey(HashAlgorithmName.SHA256, shared, output, salt, Encoding.ASCII.GetBytes("NXRP-ENROLL-KEY:" + id));
        return output;
    }

    static byte[] Aad(string id, byte[] cpub, byte[] cnonce, byte[] spub, byte[] snonce)
    {
        using var memory = new MemoryStream();
        memory.Write(Encoding.ASCII.GetBytes(id));
        foreach (var part in new[] { cpub, cnonce, spub, snonce }) memory.Write(part);
        return memory.ToArray();
    }

    static void ValidateId(string id)
    {
        if (id is null || id.Length != 5) throw new ArgumentException("Invalid identity.");
        foreach (char c in id) if (!"23456789ABCDEFGHJKLMNPQRSTUVWXYZ".Contains(c)) throw new ArgumentException("Invalid identity.");
    }

    public static async Task WriteFrame(Stream stream, ReadOnlyMemory<byte> body, CancellationToken cancel)
    {
        if (body.Length is < 1 or > MaximumFrame) throw new IOException("Invalid enrollment frame size.");
        var frame = new byte[2 + body.Length];
        BinaryPrimitives.WriteUInt16BigEndian(frame, (ushort)body.Length);
        body.CopyTo(frame.AsMemory(2));
        await stream.WriteAsync(frame, cancel).ConfigureAwait(false);
    }

    public static async Task<byte[]> ReadFrame(Stream stream, CancellationToken cancel)
    {
        var header = new byte[2];
        await stream.ReadExactlyAsync(header, cancel).ConfigureAwait(false);
        int size = BinaryPrimitives.ReadUInt16BigEndian(header);
        if (size is < 1 or > MaximumFrame) throw new IOException("Invalid enrollment frame size.");
        var body = new byte[size];
        await stream.ReadExactlyAsync(body, cancel).ConfigureAwait(false);
        return body;
    }

    public static byte[] Fingerprint(ReadOnlySpan<byte> key) => SHA256.HashData(key);

    // ---- client (desktop) ------------------------------------------------------------------------
    public sealed class Client : IDisposable
    {
        readonly string id, password;
        ECDiffieHellman? ecdh;
        byte[] cpub = Array.Empty<byte>(), cnonce = Array.Empty<byte>();

        public Client(string id, string password)
        {
            ValidateId(id);
            if (string.IsNullOrEmpty(password) || password.Length > 32) throw new ArgumentException("Invalid password.");
            this.id = id; this.password = password;
        }

        public byte[] Hello()
        {
            ecdh?.Dispose();
            ecdh = ECDiffieHellman.Create(ECCurve.NamedCurves.nistP256);
            cpub = ecdh.ExportSubjectPublicKeyInfo();
            cnonce = RandomNumberGenerator.GetBytes(NonceLength);
            var proof = Mac(password, "C1", Encoding.ASCII.GetBytes(id), cpub, cnonce);
            var hello = new byte[5 + 2 + cpub.Length + NonceLength + 32];
            Encoding.ASCII.GetBytes(id).CopyTo(hello, 0);
            BinaryPrimitives.WriteUInt16BigEndian(hello.AsSpan(5), (ushort)cpub.Length);
            cpub.CopyTo(hello, 7);
            cnonce.CopyTo(hello, 7 + cpub.Length);
            proof.CopyTo(hello, 7 + cpub.Length + NonceLength);
            return hello;
        }

        // reply = the server's body after its status byte. Verifies the server knows the password,
        // then seals the key. The caller keeps ownership of 'key'.
        public byte[] Finish(ReadOnlySpan<byte> reply, ReadOnlySpan<byte> key)
        {
            if (ecdh is null || key.Length != KeyLength) throw new InvalidOperationException("Enrollment not started.");
            if (reply.Length < 2) throw new IOException("Short enrollment reply.");
            int length = BinaryPrimitives.ReadUInt16BigEndian(reply);
            if (length is < 1 or > MaximumPublicKey || reply.Length != 2 + length + NonceLength + 32) throw new IOException("Malformed enrollment reply.");
            var spub = reply.Slice(2, length).ToArray();
            var snonce = reply.Slice(2 + length, NonceLength).ToArray();
            var mac = reply.Slice(2 + length + NonceLength, 32);
            var expected = Mac(password, "S1", Encoding.ASCII.GetBytes(id), cpub, cnonce, spub, snonce);
            if (!CryptographicOperations.FixedTimeEquals(expected, mac)) throw new IOException("The rendezvous server could not prove it knows this console's password.");

            byte[] encryptionKey;
            using (var server = ECDiffieHellman.Create())
            {
                server.ImportSubjectPublicKeyInfo(spub, out _);
                var shared = ecdh.DeriveRawSecretAgreement(server.PublicKey);
                try { encryptionKey = DeriveKey(shared, cnonce, snonce, id); }
                finally { CryptographicOperations.ZeroMemory(shared); }
            }
            var nonce = RandomNumberGenerator.GetBytes(12);
            var cipher = new byte[KeyLength]; var tag = new byte[16];
            try
            {
                using var gcm = new AesGcm(encryptionKey, 16);
                gcm.Encrypt(nonce, key, cipher, tag, Aad(id, cpub, cnonce, spub, snonce));
            }
            finally { CryptographicOperations.ZeroMemory(encryptionKey); }
            var proof = Mac(password, "C2", Encoding.ASCII.GetBytes(id), cpub, cnonce, spub, snonce, nonce, cipher, tag);
            var finish = new byte[12 + KeyLength + 16 + 32];
            nonce.CopyTo(finish, 0); cipher.CopyTo(finish, 12); tag.CopyTo(finish, 12 + KeyLength); proof.CopyTo(finish, 12 + KeyLength + 16);
            return finish;
        }

        public void Dispose() { ecdh?.Dispose(); ecdh = null; }
    }

    // ---- server ----------------------------------------------------------------------------------
    public sealed class Server : IDisposable
    {
        readonly ECDiffieHellman ecdh = ECDiffieHellman.Create(ECCurve.NamedCurves.nistP256);
        string id = "", password = "";
        byte[] cpub = Array.Empty<byte>(), cnonce = Array.Empty<byte>(), spub = Array.Empty<byte>(), snonce = Array.Empty<byte>();

        // Parses only the claimed id so the caller can look up that session's password.
        public static bool TryReadId(ReadOnlySpan<byte> hello, out string id)
        {
            id = "";
            if (hello.Length < 5) return false;
            try { id = Encoding.ASCII.GetString(hello[..5]); ValidateId(id); return true; }
            catch (ArgumentException) { id = ""; return false; }
        }

        // Verifies the client's password proof; returns the reply body (without the status byte) or null.
        public byte[]? Accept(ReadOnlySpan<byte> hello, string sessionPassword)
        {
            if (!TryReadId(hello, out id) || hello.Length < 7) return null;
            int length = BinaryPrimitives.ReadUInt16BigEndian(hello[5..]);
            if (length is < 1 or > MaximumPublicKey || hello.Length != 7 + length + NonceLength + 32) return null;
            cpub = hello.Slice(7, length).ToArray();
            cnonce = hello.Slice(7 + length, NonceLength).ToArray();
            var proof = hello.Slice(7 + length + NonceLength, 32);
            password = sessionPassword;
            var expected = Mac(password, "C1", Encoding.ASCII.GetBytes(id), cpub, cnonce);
            if (!CryptographicOperations.FixedTimeEquals(expected, proof)) return null;
            try { using var probe = ECDiffieHellman.Create(); probe.ImportSubjectPublicKeyInfo(cpub, out _); }
            catch (CryptographicException) { return null; }
            spub = ecdh.ExportSubjectPublicKeyInfo();
            snonce = RandomNumberGenerator.GetBytes(NonceLength);
            var mac = Mac(password, "S1", Encoding.ASCII.GetBytes(id), cpub, cnonce, spub, snonce);
            var reply = new byte[2 + spub.Length + NonceLength + 32];
            BinaryPrimitives.WriteUInt16BigEndian(reply, (ushort)spub.Length);
            spub.CopyTo(reply, 2); snonce.CopyTo(reply, 2 + spub.Length); mac.CopyTo(reply, 2 + spub.Length + NonceLength);
            return reply;
        }

        // Returns the decrypted 32-byte key (caller must zero it) or null if the proof or tag is wrong.
        public byte[]? Open(ReadOnlySpan<byte> finish)
        {
            if (finish.Length != 12 + KeyLength + 16 + 32 || spub.Length == 0) return null;
            var nonce = finish[..12].ToArray();
            var cipher = finish.Slice(12, KeyLength).ToArray();
            var tag = finish.Slice(12 + KeyLength, 16).ToArray();
            var proof = finish.Slice(12 + KeyLength + 16, 32);
            var expected = Mac(password, "C2", Encoding.ASCII.GetBytes(id), cpub, cnonce, spub, snonce, nonce, cipher, tag);
            if (!CryptographicOperations.FixedTimeEquals(expected, proof)) return null;
            byte[] encryptionKey;
            using (var client = ECDiffieHellman.Create())
            {
                client.ImportSubjectPublicKeyInfo(cpub, out _);
                var shared = ecdh.DeriveRawSecretAgreement(client.PublicKey);
                try { encryptionKey = DeriveKey(shared, cnonce, snonce, id); }
                finally { CryptographicOperations.ZeroMemory(shared); }
            }
            var key = new byte[KeyLength];
            try
            {
                using var gcm = new AesGcm(encryptionKey, 16);
                gcm.Decrypt(nonce, cipher, tag, key, Aad(id, cpub, cnonce, spub, snonce));
                return key;
            }
            catch (CryptographicException) { CryptographicOperations.ZeroMemory(key); return null; }
            finally { CryptographicOperations.ZeroMemory(encryptionKey); }
        }

        public void Dispose() => ecdh.Dispose();
    }
}
