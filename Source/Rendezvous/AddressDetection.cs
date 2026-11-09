using System.Net;
using System.Net.Http;
using System.Net.Sockets;

namespace Rendezvous;

// Resolve advertised addresses from local routing information for the startup banner.
public static class AddressDetection
{
	// The LAN-facing address other devices on the same network reach this machine at. Opens a
	// UDP "connection" without sending anything - just consults the local routing table.
	public static string? DetectLocalIp()
	{
		try
		{
			using var socket = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
			socket.Connect("8.8.8.8", 65530);
			return (socket.LocalEndPoint as IPEndPoint)?.Address.ToString();
		}
		catch
		{
			return null;
		}
	}

	// The address this machine is reached at from the open internet, via a public HTTP echo
	// service. Best-effort: null on any failure.
	public static async Task<string?> DetectPublicIp(CancellationToken cancel = default)
	{
		try
		{
			using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(4) };
			string ip = await http.GetStringAsync("https://api.ipify.org", cancel);
			return IPAddress.TryParse(ip.Trim(), out var address) && IsPublic(address) ? address.ToString() : null;
		}
		catch
		{
			return null;
		}
	}
    public static bool IsPublic(IPAddress address)
    {
        if(address.AddressFamily != AddressFamily.InterNetwork) return false;
        var b=address.GetAddressBytes();
        return b[0]!=0 && b[0]!=10 && b[0]!=127 && b[0]<224 &&
            !(b[0]==169&&b[1]==254) && !(b[0]==172&&b[1]>=16&&b[1]<=31) &&
            !(b[0]==192&&b[1]==168) && !(b[0]==100&&b[1]>=64&&b[1]<=127) &&
            !(b[0]==192&&b[1]==0&&b[2]==0) && !(b[0]==192&&b[1]==0&&b[2]==2) &&
            !(b[0]==198&&(b[1]==18||b[1]==19)) && !(b[0]==198&&b[1]==51&&b[2]==100) &&
            !(b[0]==203&&b[1]==0&&b[2]==113);
    }
}
