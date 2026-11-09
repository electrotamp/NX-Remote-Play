using System;

namespace NxRemotePlay.Client.Core
{
    // Server selection: a toggle. ON = the default server (play.nxremoteplay.com, port 7921).
    // OFF = the user's own server: any IPv4 address or DNS name with an optional ":port" (7921 when omitted), used
    // exactly as typed. Same rules as the Switch side (server_config.h) and the Android app (Prefs.parseServerAddress).
    // The application only has to resolve the name (PunchSource does, with Dns.GetHostAddressesAsync).
    public static class ServerAddress
    {
        public const string DefaultName = "play.nxremoteplay.com";
        public const int DefaultPort = 7921;

        // Validates "host[:port]". host is returned as typed. False if it is not a valid IPv4 address or DNS name.
        public static bool TryParse(string? text, out string host, out int port)
        {
            host = ""; port = DefaultPort;
            if (string.IsNullOrWhiteSpace(text)) return false;
            var s = text.Trim();
            foreach (var c in s) if (char.IsWhiteSpace(c)) return false;
            var colon = s.LastIndexOf(':');
            var name = s;
            if (colon >= 0)
            {
                name = s[..colon];
                var p = s[(colon + 1)..];
                if (p.Length is < 1 or > 5) return false;
                foreach (var c in p) if (c is < '0' or > '9') return false;
                port = int.Parse(p);
                if (port is < 1 or > 65535) return false;
            }
            if (name.Length is < 1 or > 60) return false;
            foreach (var label in name.Split('.'))
            {
                if (label.Length == 0 || label[0] == '-' || label[^1] == '-') return false;
                foreach (var c in label)
                    if (!(c is >= '0' and <= '9' || c is >= 'a' and <= 'z' || c is >= 'A' and <= 'Z' || c == '-')) return false;
            }
            // Dotted numbers must be a real IPv4 address, not a name that merely looks like one.
            var numeric = true;
            foreach (var c in name) if (c != '.' && (c < '0' || c > '9')) numeric = false;
            if (numeric)
            {
                var octets = name.Split('.');
                if (octets.Length != 4) return false;
                foreach (var o in octets) if (o.Length > 3 || int.Parse(o) > 255) return false;
            }
            host = name;
            return true;
        }

        public static bool IsDefault(string? text) =>
            TryParse(text, out var h, out var p) && h.Equals(DefaultName, StringComparison.OrdinalIgnoreCase) && p == DefaultPort;

        // The server to use right now: "host:port" with the explicit port.
        public static string Effective(Options o)
        {
            var useDefault = o.UseDefaultServer ?? (string.IsNullOrWhiteSpace(o.LastRelayServer) || IsDefault(o.LastRelayServer));
            var custom = !string.IsNullOrWhiteSpace(o.CustomServer) ? o.CustomServer
                : (o.UseDefaultServer == null ? o.LastRelayServer : "");   // older settings stored only one server text
            if (!useDefault && TryParse(custom, out var host, out var port)) return $"{host}:{port}";
            return $"{DefaultName}:{DefaultPort}";
        }

        public static bool UsesDefault(Options o) =>
            o.UseDefaultServer ?? (string.IsNullOrWhiteSpace(o.LastRelayServer) || IsDefault(o.LastRelayServer));
    }
}
