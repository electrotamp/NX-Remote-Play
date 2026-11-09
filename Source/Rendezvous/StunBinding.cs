using System.Buffers.Binary;
using System.Net;

namespace Rendezvous;

// RFC 8489 discovery only: no allocation, permissions, channels or data relay.
internal static class StunBinding
{
    const uint Cookie=0x2112A442;
    internal static bool IsStun(ReadOnlySpan<byte> bytes)=>bytes.Length>=20&&
        (bytes[0]&0xC0)==0&&BinaryPrimitives.ReadUInt32BigEndian(bytes[4..])==Cookie;
    internal static byte[]? Reply(ReadOnlySpan<byte> request,IPEndPoint observed)
    {
        if(!IsStun(request)||request.Length>1024||BinaryPrimitives.ReadUInt16BigEndian(request)!=1||
            BinaryPrimitives.ReadUInt16BigEndian(request[2..])!=request.Length-20||(request.Length&3)!=0) return null;
        var unknown=new HashSet<ushort>();
        for(int at=20;at<request.Length;) {
            if(at+4>request.Length) return null;
            ushort type=BinaryPrimitives.ReadUInt16BigEndian(request[at..]);
            int length=BinaryPrimitives.ReadUInt16BigEndian(request[(at+2)..]);
            int end=at+4+((length+3)&~3);if(end>request.Length) return null;
            // Authentication is not negotiated by this discovery service.
            if(type<0x8000) unknown.Add(type);
            if(type==0x8028&&(length!=4||end!=request.Length||
                BinaryPrimitives.ReadUInt32BigEndian(request[(at+4)..])!=(Crc(request[..at])^0x5354554e))) return null;
            at=end;
        }
        byte[] result;
        if(unknown.Count>0) {
            int listLength=unknown.Count*2;
            result=new byte[20+8+4+((listLength+3)&~3)+8];
            BinaryPrimitives.WriteUInt16BigEndian(result,0x0111);
            BinaryPrimitives.WriteUInt16BigEndian(result.AsSpan(20),0x0009);
            BinaryPrimitives.WriteUInt16BigEndian(result.AsSpan(22),4);
            result[26]=4;result[27]=20; // 420 Unknown Attribute
            BinaryPrimitives.WriteUInt16BigEndian(result.AsSpan(28),0x000A);
            BinaryPrimitives.WriteUInt16BigEndian(result.AsSpan(30),(ushort)listLength);
            int at=32;foreach(ushort type in unknown.Order()) {BinaryPrimitives.WriteUInt16BigEndian(result.AsSpan(at),type);at+=2;}
        } else {
            byte[] address=observed.Address.GetAddressBytes();
            if(address.Length is not (4 or 16)) return null;
            result=new byte[20+8+address.Length+8];
            BinaryPrimitives.WriteUInt16BigEndian(result,0x0101);
            BinaryPrimitives.WriteUInt16BigEndian(result.AsSpan(20),0x0020);
            BinaryPrimitives.WriteUInt16BigEndian(result.AsSpan(22),(ushort)(4+address.Length));
            result[25]=(byte)(address.Length==4?1:2);
            BinaryPrimitives.WriteUInt16BigEndian(result.AsSpan(26),(ushort)(observed.Port^(Cookie>>16)));
            for(int i=0;i<address.Length;i++) result[28+i]=(byte)(address[i]^request[4+i]);
        }
        BinaryPrimitives.WriteUInt16BigEndian(result.AsSpan(2),(ushort)(result.Length-20));
        request[4..20].CopyTo(result.AsSpan(4));
        int fingerprint=result.Length-8;
        BinaryPrimitives.WriteUInt16BigEndian(result.AsSpan(fingerprint),0x8028);
        BinaryPrimitives.WriteUInt16BigEndian(result.AsSpan(fingerprint+2),4);
        BinaryPrimitives.WriteUInt32BigEndian(result.AsSpan(fingerprint+4),Crc(result.AsSpan(0,fingerprint))^0x5354554e);
        return result;
    }
    static uint Crc(ReadOnlySpan<byte> bytes)
    {
        uint crc=uint.MaxValue;
        foreach(byte value in bytes) {crc^=value;for(int bit=0;bit<8;bit++) crc=(crc>>1)^((crc&1)!=0?0xEDB88320u:0);}
        return ~crc;
    }
}
