using System;
using System.Buffers.Binary;
using System.Text;

namespace NxRemotePlay.Client.Sources;

// First-use owner-key install over the USB control channel (operations 8 and 9). Mirrors
// sysmodule/source/modes/UsbControl.h and BridgeOwner.c. The key is staged on the console, the desktop
// enrolls the same key with the rendezvous server, and only then does the console commit it. Until the
// commit nothing about the console's ordinary hosting changes.
internal static class UsbEnrollmentProtocol
{
    internal const uint ResponseMagic = 0x3145584E; // "NXE1"
    internal const uint CapabilityBit = 32;
    internal const byte StageOperation = 8, CommitOperation = 9;

    static byte[] Header(byte operation, int length, uint sequence, ulong session)
    {
        if (session == 0 || sequence == 0) throw new ArgumentException("Invalid USB enrollment frame.");
        var frame = new byte[64];
        BinaryPrimitives.WriteUInt32LittleEndian(frame, 0x3143584E);
        BinaryPrimitives.WriteUInt16LittleEndian(frame.AsSpan(4), 1);
        BinaryPrimitives.WriteUInt16LittleEndian(frame.AsSpan(6), (ushort)length);
        BinaryPrimitives.WriteUInt32LittleEndian(frame.AsSpan(8), sequence);
        frame[12] = operation;
        BinaryPrimitives.WriteUInt64LittleEndian(frame.AsSpan(16), session);
        return frame;
    }

    static void Id(string id, Span<byte> destination)
    {
        if (id is null || id.Length != 5) throw new ArgumentException("Invalid identity.");
        foreach (char c in id) if (!"23456789ABCDEFGHJKLMNPQRSTUVWXYZ".Contains(c)) throw new ArgumentException("Invalid identity.");
        Encoding.ASCII.GetBytes(id, destination);
    }

    // payload: 5-byte Session ID + 32-byte key. The response token is the frame sequence.
    internal static byte[] StageFrame(string id, ReadOnlySpan<byte> key, uint sequence, ulong session)
    {
        if (key.Length != 32) throw new ArgumentException("Owner key must be 256 bits.");
        var frame = Header(StageOperation, 37, sequence, session);
        Id(id, frame.AsSpan(24, 5));
        key.CopyTo(frame.AsSpan(29));
        return frame;
    }

    internal static byte[] CommitFrame(string id, uint sequence, ulong session)
    {
        var frame = Header(CommitOperation, 5, sequence, session);
        Id(id, frame.AsSpan(24, 5));
        return frame;
    }

    internal static string Describe(uint status) => status switch
    {
        1 => "The Switch did not accept key setup (firmware too old, or setup not requested).",
        2 => "The Switch's Session ID changed during setup; reconnect USB to try again.",
        3 => "The Switch already has an owner key for this Session ID.",
        4 => "The Switch could not store the owner key on its SD card.",
        5 => "The Switch has no staged owner key to commit.",
        _ => "The Switch rejected owner key setup (status " + status + ")."
    };

    // Same 82-byte envelope as the owner-proof reply; the 32 payload bytes carry SHA-256(key).
    internal static (uint Token, uint Status, byte[] Fingerprint) Response(ReadOnlySpan<byte> packet)
    {
        if (packet.Length != 82 || BinaryPrimitives.ReadUInt32LittleEndian(packet) != ResponseMagic ||
            BinaryPrimitives.ReadUInt32LittleEndian(packet.Slice(4)) != 64 ||
            BinaryPrimitives.ReadUInt64LittleEndian(packet.Slice(8)) != 0 || packet[16] != 0 || packet[17] != 0)
            throw new ArgumentException("Invalid USB enrollment response envelope.");
        uint token = BinaryPrimitives.ReadUInt32LittleEndian(packet.Slice(18));
        uint status = BinaryPrimitives.ReadUInt32LittleEndian(packet.Slice(22));
        if (token == 0 || status > 8) throw new ArgumentException("Invalid USB enrollment response status.");
        for (int i = 58; i < 82; i++) if (packet[i] != 0) throw new ArgumentException("Invalid USB enrollment response padding.");
        if (status != 0)
            for (int i = 26; i < 58; i++) if (packet[i] != 0) throw new ArgumentException("Failed USB enrollment response contains a fingerprint.");
        return (token, status, packet.Slice(26, 32).ToArray());
    }
}
