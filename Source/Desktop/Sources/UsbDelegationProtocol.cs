using System;
using System.Buffers.Binary;

namespace NxRemotePlay.Client.Sources;
internal static class UsbDelegationProtocol
{
    internal const uint ResponseMagic = 0x314C584E;
    internal static byte[] Frame(byte operation, ulong generation, uint milliseconds, uint token, uint sequence, ulong session)
    {
        if (operation > 2 || generation == 0 || token == 0 || session == 0 ||
            (operation == 2 ? milliseconds != 0 : milliseconds is 0 or > 20000)) throw new ArgumentException("Invalid USB delegation.");
        var frame = new byte[64]; BinaryPrimitives.WriteUInt32LittleEndian(frame, 0x3143584E);
        BinaryPrimitives.WriteUInt16LittleEndian(frame.AsSpan(4), 1); BinaryPrimitives.WriteUInt16LittleEndian(frame.AsSpan(6), 24);
        BinaryPrimitives.WriteUInt32LittleEndian(frame.AsSpan(8), sequence); frame[12] = 7;
        BinaryPrimitives.WriteUInt64LittleEndian(frame.AsSpan(16), session);
        BinaryPrimitives.WriteUInt32LittleEndian(frame.AsSpan(24), token); frame[28] = operation;
        BinaryPrimitives.WriteUInt64LittleEndian(frame.AsSpan(32), generation);
        BinaryPrimitives.WriteUInt32LittleEndian(frame.AsSpan(40), milliseconds); return frame;
    }
    internal static void ValidateAcknowledgement(byte[] bytes, byte operation, ulong generation, uint milliseconds)
    {
        if (bytes.Length != 32 || BinaryPrimitives.ReadUInt64LittleEndian(bytes) != generation ||
            BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(8)) != milliseconds || bytes[12] != operation)
            throw new ArgumentException("USB delegation acknowledgement changed the requested lease.");
        for (int at = 13; at < bytes.Length; at++) if (bytes[at] != 0) throw new ArgumentException("Invalid USB delegation acknowledgement padding.");
    }
}
