using System;
using System.Net;

namespace NXRemotePlay.Protocol;

// URL generation never embeds a pairing password or substitutes a DNS name.
public static class BrowserJoinLink
{
    // Session join link: the token rides in the URL fragment, which browsers never send to a server and the web page removes at once.
    public static Uri CreateSession(Uri link, string token)
    {
        if (link is null || token is null || token.Length != 64) throw new ArgumentException("Missing link or token.");
        foreach (char c in token) if (!Uri.IsHexDigit(c)) throw new ArgumentException("Invalid token.");
        return new UriBuilder(link) { Fragment = "t=" + token.ToUpperInvariant() }.Uri;
    }
    public static Uri Create(IPAddress address, int port, string sessionId, bool loopbackTest = false)
    {
        if (address is null || address.Equals(IPAddress.Any) || address.Equals(IPAddress.IPv6Any) ||
            port is < 1 or > 65535 || sessionId is null || sessionId.Length != 5)
            throw new ArgumentException("Missing browser address, port or session ID.");
        foreach (char c in sessionId) if (!char.IsAsciiLetterOrDigit(c)) throw new ArgumentException("Invalid session ID.");
        if (loopbackTest && !IPAddress.IsLoopback(address)) throw new ArgumentException("HTTP browser fixtures must stay on loopback.");
        return new UriBuilder(loopbackTest ? "http" : "https", address.ToString(), port, "/")
        {
            Query = "session=" + sessionId.ToUpperInvariant()
        }.Uri;
    }
}
