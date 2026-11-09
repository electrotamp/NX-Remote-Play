using NXRemotePlay.Protocol;
using System.Text.RegularExpressions;

namespace Rendezvous;
internal sealed class P2pIceConfiguration
{
    readonly string[] urls;
    internal P2pIceConfiguration(string[] urls)
    {
        if(urls.Length>8||urls.Sum(value=>value.Length)>1024) throw new ArgumentException("Too many ICE endpoints.");
        foreach(string url in urls) {
            if(!Regex.IsMatch(url,@"^stun:([a-zA-Z0-9.-]+|\[[0-9A-Fa-f:]+\])(:[0-9]{1,5})?$",RegexOptions.CultureInvariant))
                throw new ArgumentException("Invalid ICE endpoint.");
            var port=Regex.Match(url,@":([0-9]+)(?:\?|$)");
            if(port.Success&&(!int.TryParse(port.Groups[1].Value,out int number)||number is <1 or >65535)) throw new ArgumentException("Invalid ICE port.");
        }
        this.urls=(string[])urls.Clone();
    }
    internal RtcIceServer[] For(string? host=null,int port=7921)
    {
        if(urls.Length==0&&host is not null) {
            if(Uri.CheckHostName(host.Trim('[',']'))==UriHostNameType.Unknown) throw new ArgumentException("Invalid discovery hostname.");
            if(host.Contains(':')) host="["+host.Trim('[',']')+"]";
            return [new RtcIceServer(["stun:"+host+":"+port])];
        }
        return urls.Select(url=>new RtcIceServer([url])).ToArray();
    }
}
