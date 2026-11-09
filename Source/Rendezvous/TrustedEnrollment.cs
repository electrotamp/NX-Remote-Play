using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;
namespace Rendezvous;
internal static class TrustedEnrollment
{
    internal static void Import(string registry,string path)
    {
        var record=File.ReadAllBytes(path);
        try{
            if(record.Length!=48||BinaryPrimitives.ReadUInt32LittleEndian(record)!=0x314B584E||BinaryPrimitives.ReadUInt16LittleEndian(record.AsSpan(4))!=1||BinaryPrimitives.ReadUInt16LittleEndian(record.AsSpan(6))!=48||record[13]!=0||record[14]!=0||record[15]!=0)
                throw new InvalidDataException("Invalid trusted console enrollment record.");
            string id=Encoding.ASCII.GetString(record,8,5);
            using var owners=new HostOwnership(registry);owners.Enroll(id,record.AsSpan(16,32));
        }finally{CryptographicOperations.ZeroMemory(record);}
    }
}
