using System;
using System.Buffers.Binary;

namespace NxRemotePlay.Client.Sources;

internal static class UsbOwnerProtocol
{
    internal const uint ResponseMagic = 0x3152584E;
    internal const int MaximumRequest = 640;
    internal static byte[] Fragment(ReadOnlySpan<byte> request, uint token, int offset, uint sequence, ulong session)
    {
        if (token == 0 || session == 0 || request.Length is < 1 or > MaximumRequest ||
            offset < 0 || offset >= request.Length || offset % 32 != 0)
            throw new ArgumentException("Invalid USB owner request fragment.");
        int count = Math.Min(32, request.Length - offset);
        var frame = new byte[64];
        BinaryPrimitives.WriteUInt32LittleEndian(frame, 0x3143584E);
        BinaryPrimitives.WriteUInt16LittleEndian(frame.AsSpan(4), 1);
        BinaryPrimitives.WriteUInt16LittleEndian(frame.AsSpan(6), (ushort)(8 + count));
        BinaryPrimitives.WriteUInt32LittleEndian(frame.AsSpan(8), sequence);
        frame[12] = 6;
        BinaryPrimitives.WriteUInt64LittleEndian(frame.AsSpan(16), session);
        BinaryPrimitives.WriteUInt32LittleEndian(frame.AsSpan(24), token);
        BinaryPrimitives.WriteUInt16LittleEndian(frame.AsSpan(28), (ushort)request.Length);
        BinaryPrimitives.WriteUInt16LittleEndian(frame.AsSpan(30), (ushort)offset);
        request.Slice(offset, count).CopyTo(frame.AsSpan(32));
        return frame;
    }
    internal static (uint Token, uint Status, byte[] Proof) Response(ReadOnlySpan<byte> packet, uint magic = ResponseMagic)
    {
        if (packet.Length != 82 || BinaryPrimitives.ReadUInt32LittleEndian(packet) != magic ||
            BinaryPrimitives.ReadUInt32LittleEndian(packet.Slice(4)) != 64 ||
            BinaryPrimitives.ReadUInt64LittleEndian(packet.Slice(8)) != 0 || packet[16] != 0 || packet[17] != 0)
            throw new ArgumentException("Invalid USB owner response envelope.");
        uint token = BinaryPrimitives.ReadUInt32LittleEndian(packet.Slice(18));
        uint status = BinaryPrimitives.ReadUInt32LittleEndian(packet.Slice(22));
        if (token == 0 || status > 2) throw new ArgumentException("Invalid USB owner response status.");
        for (int i = 58; i < 82; i++) if (packet[i] != 0) throw new ArgumentException("Invalid USB owner response padding.");
        if (status != 0)
            for (int i = 26; i < 58; i++) if (packet[i] != 0) throw new ArgumentException("Failed USB owner response contains a proof.");
        return (token, status, packet.Slice(26, 32).ToArray());
    }
}
