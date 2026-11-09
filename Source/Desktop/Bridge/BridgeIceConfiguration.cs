using NXRemotePlay.Protocol;
using SIPSorcery.Net;
using System;
using System.Collections.Generic;

namespace NxRemotePlay.Client.Bridge;
internal static class BridgeIceConfiguration
{
    internal static RTCConfiguration Create(RtcIceServer[]? servers, bool relayOnly)
    {
        if (relayOnly) throw new ArgumentException("Relay transport is not supported.");
        if (servers is not null && servers.Length > 8) throw new ArgumentException("Too many ICE servers.");
        var result = new RTCConfiguration { iceServers = new List<RTCIceServer>(), iceTransportPolicy = RTCIceTransportPolicy.all };
        if (servers is not null) foreach (var server in servers)
            {
                if (server.Urls is null || server.Urls.Length is < 1 or > 8) throw new ArgumentException("Invalid ICE server.");
                foreach (string url in server.Urls)
                {
                    if (url.Length > 256 || !System.Text.RegularExpressions.Regex.IsMatch(url, @"^stun:([a-zA-Z0-9.-]+|\[[0-9A-Fa-f:]+\])(:[0-9]{1,5})?$")) throw new ArgumentException("Only STUN discovery is supported.");
                    var port = System.Text.RegularExpressions.Regex.Match(url, @":([0-9]+)$");
                    if (port.Success && (!int.TryParse(port.Groups[1].Value, out int number) || number is < 1 or > 65535)) throw new ArgumentException("Invalid STUN port.");
                    if (!string.IsNullOrEmpty(server.Username) || !string.IsNullOrEmpty(server.Credential)) throw new ArgumentException("ICE relay credentials are not supported.");
                    result.iceServers.Add(new RTCIceServer { urls = url });
                }
            }
        return result;
    }
}
