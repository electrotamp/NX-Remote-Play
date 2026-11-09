using System.Security.Cryptography;

namespace Rendezvous;

// Issued only by authenticated HTTPS host ownership operations. Consuming a ticket
// does not authorize a socket by itself: admission also checks Current under the
// ownership gate, so expiry, release and a new generation fence delayed sockets.
internal sealed class BridgePunchTickets
{
    internal sealed record Ticket(string Id,string Owner,ulong Generation,DateTimeOffset ExpiresAt);
    readonly object gate=new();
    readonly Dictionary<string,Ticket> tickets=new();
    internal string? Issue(HostRoute route)
    {
        if(route.Kind!=HostKind.UsbBridge||route.ExpiresAt<=DateTimeOffset.UtcNow) return null;
        lock(gate) {
            foreach(var key in tickets.Keys.ToArray()) if(tickets[key].ExpiresAt<=DateTimeOffset.UtcNow) tickets.Remove(key);
            if(tickets.Count>=2048) return null;
            string token=Convert.ToHexString(RandomNumberGenerator.GetBytes(32));
            tickets.Add(token,new(route.Id,route.Owner,route.Generation,DateTimeOffset.UtcNow.AddSeconds(10)));
            return token;
        }
    }
    internal Ticket? Consume(string id,string token)
    {
        lock(gate) {
            if(!tickets.Remove(token,out var scope)||scope.Id!=id||scope.ExpiresAt<=DateTimeOffset.UtcNow) return null;
            return scope;
        }
    }
}
