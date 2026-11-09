using System;
using System.Net.Http;
using System.Reflection;
using System.Text.Json;
using System.Threading.Tasks;

namespace NxRemotePlay.Client.Core;

// Asks GitHub for the newest published release and compares its tag with this build's version.
// The same check (URL, tag format and version order) is implemented by the Android app, the Switch app and the
// web page; see COMPATIBILITY.md "Update check".
internal static class UpdateCheck
{
    public const string ReleasesPage = "https://github.com/ElectroTamp/NX-Remote-Play/releases";
    public const string ReleasesPageLabel = "github.com/ElectroTamp/NX-Remote-Play/releases";
    const string ReleasesApi = "https://api.github.com/repos/ElectroTamp/NX-Remote-Play/releases?per_page=10";

    public enum State { NotChecked, Checking, UpToDate, Available, Failed }

    static readonly object gate = new();
    static Task? running;
    public static volatile State Current = State.NotChecked;
    public static string Latest = "";

    public static string Version { get; } = RunningVersion();

    static string RunningVersion()
    {
        string version = typeof(UpdateCheck).Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion ?? "0.0.0";
        int build = version.IndexOf('+');
        return build >= 0 ? version[..build] : version;
    }

    public static string StatusText => Current switch
    {
        State.Checking => "Checking...",
        State.UpToDate => "Up to date",
        State.Available => "Update available: " + Latest,
        State.Failed => "Could not check",
        _ => "Not checked",
    };

    // Starts one background check; a call while a check is running does nothing.
    public static void Start()
    {
        lock (gate)
        {
            if (running is { IsCompleted: false }) return;
            Current = State.Checking;
            running = Task.Run(Run);
        }
    }

    static async Task Run()
    {
        try
        {
            using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(10) };
            http.DefaultRequestHeaders.UserAgent.ParseAdd("NX-Remote-Play-Desktop/" + Version);
            http.DefaultRequestHeaders.Accept.ParseAdd("application/vnd.github+json");
            using var response = await http.GetAsync(ReleasesApi);
            response.EnsureSuccessStatusCode();
            using var json = JsonDocument.Parse(await response.Content.ReadAsStringAsync());
            string newest = "";
            foreach (var release in json.RootElement.EnumerateArray())
            {
                if (release.TryGetProperty("draft", out var draft) && draft.ValueKind == JsonValueKind.True) continue;
                if (!release.TryGetProperty("tag_name", out var tag) || tag.GetString() is not { } name) continue;
                if (Compare(name, "0.0.0") is null) continue;
                if (newest.Length == 0 || Compare(name, newest) > 0) newest = name;
            }
            if (newest.Length > 0 && Compare(newest, Version) > 0) { Latest = newest.TrimStart('v', 'V'); Current = State.Available; }
            else Current = State.UpToDate;
        }
        catch (Exception ex)
        {
            Program.LogError("update check", ex);
            Current = State.Failed;
        }
    }

    // Semantic version order (MAJOR.MINOR.PATCH, then a pre-release such as -beta.2 sorts before the release).
    // A leading "v" and any "+build" suffix are ignored. Null when either side is not a version.
    public static int? Compare(string a, string b)
    {
        if (!Parse(a, out var coreA, out var preA) || !Parse(b, out var coreB, out var preB)) return null;
        for (int i = 0; i < 3; i++)
            if (coreA[i] != coreB[i]) return coreA[i].CompareTo(coreB[i]);
        if (preA.Length == 0 || preB.Length == 0) return preA.Length == preB.Length ? 0 : preA.Length == 0 ? 1 : -1;
        for (int i = 0; i < Math.Min(preA.Length, preB.Length); i++)
        {
            bool numA = long.TryParse(preA[i], out long na), numB = long.TryParse(preB[i], out long nb);
            int order = numA && numB ? na.CompareTo(nb) : numA ? -1 : numB ? 1 : string.CompareOrdinal(preA[i], preB[i]);
            if (order != 0) return Math.Sign(order);
        }
        return preA.Length.CompareTo(preB.Length);
    }

    static bool Parse(string text, out long[] core, out string[] pre)
    {
        core = new long[3]; pre = Array.Empty<string>();
        string s = text.Trim();
        if (s.StartsWith('v') || s.StartsWith('V')) s = s[1..];
        int plus = s.IndexOf('+');
        if (plus >= 0) s = s[..plus];
        int dash = s.IndexOf('-');
        if (dash >= 0) { pre = s[(dash + 1)..].Split('.'); s = s[..dash]; }
        var parts = s.Split('.');
        if (parts.Length != 3) return false;
        for (int i = 0; i < 3; i++)
            if (!long.TryParse(parts[i], System.Globalization.NumberStyles.None, null, out core[i])) return false;
        foreach (var id in pre) if (id.Length == 0) return false;
        return true;
    }
}
