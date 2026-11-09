using System;
using System.IO;
using System.Text;

namespace NXRemotePlay.Protocol;

// Linked into server and desktop. The Switch parser mirrors this exact byte format.
public static class OwnerProofBytes
{
    public static byte[] Serialize(string id, byte action, byte kind, string owner, string endpoint,
        ulong generation, string nonce, long expiresAtUnixMilliseconds)
    {
        Validate(id, 5); Validate(owner, 128); Validate(endpoint, 256); Validate(nonce, 64);
        if (id.Length != 5 || action > 2 || kind > 1 || generation == 0 || nonce.Length != 64 ||
            expiresAtUnixMilliseconds <= 0)
            throw new ArgumentException("Invalid owner challenge.");
        foreach (char c in id) if (!"23456789ABCDEFGHJKLMNPQRSTUVWXYZ".Contains(c)) throw new ArgumentException("Invalid owner identity.");
        foreach (char c in nonce) if (!"0123456789ABCDEF".Contains(c)) throw new ArgumentException("Invalid owner nonce.");
        using var memory = new MemoryStream();
        using var writer = new BinaryWriter(memory, Encoding.UTF8, leaveOpen: true);
        writer.Write(Encoding.ASCII.GetBytes("NXRP-OWNER-1"));
        Field(writer, id); writer.Write(action); writer.Write(kind);
        Field(writer, owner); Field(writer, endpoint); writer.Write(generation);
        Field(writer, nonce); writer.Write(expiresAtUnixMilliseconds); writer.Flush();
        return memory.ToArray();
    }
    static void Validate(string value, int maximum)
    {
        if (value is null || value.Length == 0 || value.Length > maximum) throw new ArgumentException("Invalid owner field.");
        foreach (char c in value) if (c < 33 || c > 126) throw new ArgumentException("Owner fields require printable ASCII without spaces.");
    }
    static void Field(BinaryWriter writer, string value)
    {
        var bytes = Encoding.ASCII.GetBytes(value);
        writer.Write((ushort)bytes.Length); writer.Write(bytes);
    }
}
