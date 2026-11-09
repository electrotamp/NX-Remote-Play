using System.Buffers.Binary;
using System.Net;
using System.Security.Cryptography;
using System.Text;

namespace Rendezvous;

// Bounded UDP owner protocol for consoles without HTTPS support. Every response is
// root-authenticated in a separate domain; challenge/proof admission is tied to the
// observed peer and request correlation. Pairing passwords never authorize hosting.
internal sealed class DirectOwnerWire(HostOwnership ownership, Func<DateTimeOffset>? clock = null)
{
    internal const uint Magic = 0x3144584E;
    const string Prefix = "nxrp-direct-v1:";
    readonly Func<DateTimeOffset> now = clock ?? (() => DateTimeOffset.UtcNow);
    readonly object gate = new();
    sealed record Pending(OwnerChallenge Challenge, IPEndPoint Peer, ulong Correlation);
    sealed record Ticket(string Id, ulong Generation, IPAddress Address, DateTimeOffset Expires);
    readonly Dictionary<string,Pending> pending = new();
    readonly Dictionary<string,Ticket> tickets = new();
    readonly Dictionary<string,ulong> waking = new();
    readonly RateLimiter limiter = new(30,TimeSpan.FromSeconds(10));

    internal static string Endpoint(string id,string password) => Prefix + Convert.ToHexString(
        SHA256.HashData(Encoding.ASCII.GetBytes(id + "\n" + password)));
    internal static bool Accepts(HostRoute route,string password) => route.Kind == HostKind.Direct &&
        route.Owner == "switch:" + route.Id && CryptographicOperations.FixedTimeEquals(
            Encoding.ASCII.GetBytes(route.Endpoint),Encoding.ASCII.GetBytes(Endpoint(route.Id,password)));
    internal bool Wake(string id,string password)
    {
        lock(gate) {
            var route=ownership.Resolve(id);
            if(route is null||!Accepts(route,password)) return false;
            return ownership.Current(id,route.Generation,HostKind.Direct,current=> {
                waking[id]=current.Generation; return true;
            },()=>false);
        }
    }
    internal HostRoute? ConsumeTicket(string id,string ticket,IPAddress address)
    {
        lock(gate) {
            if(!tickets.Remove(ticket,out var value)||value.Id!=id||!value.Address.Equals(address)||value.Expires<=now()) return null;
            return ownership.Current<HostRoute?>(id,value.Generation,HostKind.Direct,route=>route,()=>null);
        }
    }
    internal byte[]? Handle(byte[] bytes,IPEndPoint peer)
    {
        if(bytes.Length is not (64 or 128)||BinaryPrimitives.ReadUInt32LittleEndian(bytes)!=Magic||
            bytes[4]!=1||bytes[5] is not (1 or 3)||bytes[6]>2||bytes[7]!=0||
            bytes[13]!=0||bytes[14]!=0||bytes[15]!=0||!limiter.Allow(peer.Address)) return null;
        string id=Encoding.ASCII.GetString(bytes,8,5);
        if(id.Any(c=>!"23456789ABCDEFGHJKLMNPQRSTUVWXYZ".Contains(c))||!ownership.IsProtected(id)) return null;
        ulong generation=BinaryPrimitives.ReadUInt64LittleEndian(bytes.AsSpan(16));
        ulong correlation=BinaryPrimitives.ReadUInt64LittleEndian(bytes.AsSpan(24));
        if(correlation==0) return null;
        lock(gate) {
            foreach(var item in pending.Where(x=>x.Value.Challenge.ExpiresAt<=now()).ToArray()) pending.Remove(item.Key);
            foreach(var item in tickets.Where(x=>x.Value.Expires<=now()).ToArray()) tickets.Remove(item.Key);
            if(bytes[5]==1) {
                if(bytes.Length!=64||pending.Count>=1024) return null;
                ulong current=ownership.Generation(id);
                // Claim discovers the durable generation after restart/bridge expiry.
                if(bytes[6]!=0&&generation!=current) return Reply(id,bytes[6],2,current,correlation,Array.Empty<byte>());
                string endpoint=Prefix+Convert.ToHexString(bytes.AsSpan(32,32));
                var challenge=ownership.Challenge(id,(OwnerAction)bytes[6],HostKind.Direct,"switch:"+id,endpoint,current);
                if(challenge is null) return Reply(id,bytes[6],2,current,correlation,Array.Empty<byte>());
                pending.Add(challenge.Nonce,new(challenge,peer,correlation));
                return Reply(id,bytes[6],0,current,correlation,HostOwnership.Canonical(challenge),type:2);
            }
            if(bytes.Length!=128) return null;
            string nonce=Encoding.ASCII.GetString(bytes,32,64);
            if(!pending.TryGetValue(nonce,out var request)||request.Correlation!=correlation||
                !request.Peer.Equals(peer)||request.Challenge.Id!=id||
                (byte)request.Challenge.Action!=bytes[6]||request.Challenge.Generation!=generation) return null;
            pending.Remove(nonce);
            var route=ownership.Authenticate(request.Challenge,bytes.AsSpan(96,32));
            if(route is null) return Reply(id,bytes[6],2,ownership.Generation(id),correlation,Array.Empty<byte>());
            var payload=new byte[72];
            if(bytes[6]!=2) {
                BinaryPrimitives.WriteUInt32LittleEndian(payload,(uint)Math.Clamp((route.ExpiresAt-now()).TotalMilliseconds,0,15000));
                if(waking.TryGetValue(id,out var wake)&&wake==route.Generation) {
                    payload[4]=1; waking.Remove(id);
                }
                // One-use host signaling admission, pinned to IP + generation + lease.
                string ticket=Convert.ToHexString(RandomNumberGenerator.GetBytes(32));
                if(tickets.Count>=1024) tickets.Remove(tickets.Keys.First());
                tickets.Add(ticket,new(id,route.Generation,peer.Address,route.ExpiresAt));
                Encoding.ASCII.GetBytes(ticket).CopyTo(payload,8);
            } else waking.Remove(id);
            return Reply(id,bytes[6],0,route.Generation,correlation,payload);
        }
    }
    byte[] Reply(string id,byte action,byte status,ulong generation,ulong correlation,byte[] payload,byte type=4)
    {
        var reply=new byte[32+payload.Length+32];
        BinaryPrimitives.WriteUInt32LittleEndian(reply,Magic); reply[4]=1; reply[5]=type; reply[6]=action; reply[7]=status;
        Encoding.ASCII.GetBytes(id).CopyTo(reply,8);
        BinaryPrimitives.WriteUInt64LittleEndian(reply.AsSpan(16),generation);
        BinaryPrimitives.WriteUInt64LittleEndian(reply.AsSpan(24),correlation);
        payload.CopyTo(reply,32);
        ownership.DirectReplyProof(id,reply.AsSpan(0,reply.Length-32)).CopyTo(reply,reply.Length-32);
        return reply;
    }
}
