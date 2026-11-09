using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using NXRemotePlay.Protocol;

namespace Rendezvous;

public enum HostKind { Direct, UsbBridge }
public enum OwnerAction { Claim, Renew, Release }

// Immutable snapshots prevent callers from changing the active route after authorization.
public sealed record HostRoute(string Id, HostKind Kind, string Owner, string Endpoint,
    ulong Generation, DateTimeOffset ExpiresAt);
public sealed record OwnerChallenge(string Id, OwnerAction Action, HostKind Kind, string Owner,
    string Endpoint, ulong Generation, string Nonce, DateTimeOffset ExpiresAt);

// Ownership core only: no network listener and no legacy password enrollment. A trusted local
// enrollment step supplies the 256-bit console key. The USB delegation and secure signaling
// adapters must be built before this can replace the live rendezvous route.
public sealed class HostOwnership : IDisposable
{
    internal readonly BridgePunchTickets PunchTickets=new();
    // Unconfirmed: the key was enrolled through the pairing channel (EnrollFromPairing) and has not yet
    // authenticated anything. Until it does, it neither fences the legacy password path (so a half-finished
    // enrollment cannot take a console offline) nor stops a later pairing-authenticated enrollment replacing
    // it (so an interrupted attempt can simply be retried). Keys imported by a trusted local record
    // default to confirmed, as does every record written before this field existed.
    sealed record Identity(string Key, ulong Generation, bool Unconfirmed = false);
    readonly object gate = new();
    readonly string path;
    readonly FileStream stateLock;
    readonly Func<DateTimeOffset> now;
    Dictionary<string, Identity> identities = new();
    readonly Dictionary<string, HostRoute> routes = new();
    readonly Dictionary<string, OwnerChallenge> challenges = new();
    readonly HashSet<string> pendingIds = new();
    bool disposed;
    public static readonly TimeSpan LeaseDuration = TimeSpan.FromSeconds(15);
    public static readonly TimeSpan ChallengeDuration = TimeSpan.FromSeconds(10);
    const int MaximumChallenges = 1024;

    public HostOwnership(string statePath, Func<DateTimeOffset>? clock = null)
    {
        path = Path.GetFullPath(statePath);
        now = clock ?? (() => DateTimeOffset.UtcNow);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        // Two service instances must not independently issue generations for one registry.
        stateLock = new FileStream(path + ".lock", FileMode.OpenOrCreate, FileAccess.ReadWrite,
            FileShare.None);
        try { Load(); }
        catch { stateLock.Dispose(); throw; }
    }

    void Load()
    {
        // Invalid/missing durable state is never replaced with a permissive empty registry.
        identities = File.Exists(path)
            ? JsonSerializer.Deserialize<Dictionary<string, Identity>>(File.ReadAllText(path))
                ?? throw new InvalidDataException("Invalid ownership state")
            : new();
        foreach (var (id, identity) in identities)
        {
            ValidateId(id);
            if (Convert.FromBase64String(identity.Key).Length != 32)
                throw new InvalidDataException("Invalid owner key");
        }
        // Routes, challenges and tokens never survive a restart. Advance the durable generation
        // before accepting any new claim, so old messages cannot renew a restarted server.
        if (identities.Count > 0)
        {
            var bumped = identities.ToDictionary(x => x.Key,
                x => x.Value with { Generation = checked(x.Value.Generation + 1) });
            Save(bumped);
            identities = bumped;
        }
    }

    // Administrative/local enrollment only. Never expose this method to pairing-password requests.
    // Rotation invalidates every route/challenge before a new key can be used.
    public void Provision(string id, ReadOnlySpan<byte> key)
    {
        ValidateId(id);
        if (key.Length != 32) throw new ArgumentException("Owner key must be 256 bits");
        lock (gate)
        {
            CheckOpen();
            identities.TryGetValue(id, out var previous);
            var candidate = new Dictionary<string, Identity>(identities)
            {
                [id] = new(Convert.ToBase64String(key), checked((previous?.Generation ?? 0) + 1))
            };
            Save(candidate);
            identities = candidate;
            routes.Remove(id);
            RemoveChallenges(id);
        }
    }

    public bool IsProtected(string id)
    {
        lock (gate) { CheckOpen(); return identities.ContainsKey(id); }
    }
    // Administrative import is idempotent and never silently rotates an existing key.
    internal void Enroll(string id,ReadOnlySpan<byte> key)
    {
        lock(gate){
            CheckOpen();ValidateId(id);if(key.Length!=32)throw new ArgumentException("Invalid enrollment key.");
            if(identities.TryGetValue(id,out var known)){
                var previous=Convert.FromBase64String(known.Key);
                try{if(!CryptographicOperations.FixedTimeEquals(previous,key))throw new InvalidOperationException("Identity already has a different owner key; explicit rotation is required.");}
                finally{CryptographicOperations.ZeroMemory(previous);}
                return;
            }
            Provision(id,key);
        }
    }

    // First-use enrollment through the pairing channel (the caller has already proven it knows the
    // live Session ID + password). The key is accepted only while the ID has no confirmed key:
    //   - unknown ID: stored, unconfirmed;
    //   - unconfirmed key: replaced (an interrupted earlier attempt can be retried);
    //   - confirmed key: refused unless it is the very same key (a lost reply can be retried).
    // Once the Switch (or a claim signed with the key) authenticates, the key is confirmed and this
    // path can no longer touch it. Returns false when the ID is locked by a different confirmed key.
    internal bool EnrollFromPairing(string id, ReadOnlySpan<byte> key)
    {
        lock (gate)
        {
            CheckOpen(); ValidateId(id);
            if (key.Length != 32) throw new ArgumentException("Invalid enrollment key.");
            if (identities.TryGetValue(id, out var known))
            {
                var previous = Convert.FromBase64String(known.Key);
                try
                {
                    if (CryptographicOperations.FixedTimeEquals(previous, key)) return true;
                    if (!known.Unconfirmed) return false;
                }
                finally { CryptographicOperations.ZeroMemory(previous); }
            }
            identities.TryGetValue(id, out var before);
            var candidate = new Dictionary<string, Identity>(identities)
            {
                [id] = new(Convert.ToBase64String(key), checked((before?.Generation ?? 0) + 1), true)
            };
            Save(candidate);
            identities = candidate;
            routes.Remove(id);
            RemoveChallenges(id);
            return true;
        }
    }

    internal bool IsConfirmed(string id)
    {
        lock (gate) { CheckOpen(); return identities.TryGetValue(id, out var known) && !known.Unconfirmed; }
    }
    // Password-only (legacy) traffic is fenced off only by a confirmed key; see Identity.Unconfirmed.
    // IsProtected still answers true for an unconfirmed key so the console can authenticate with it.
    internal bool IsFenced(string id) => IsConfirmed(id);

    // Legacy admission and ownership mutations use the same lock. The callback must be short
    // and synchronous; never hold this gate across network waits. Protection persists with no
    // live route, including expiry/restart, so delayed password-only traffic stays fenced. A key
    // that has never authenticated (see Identity.Unconfirmed) does not fence anything yet.
    internal T Legacy<T>(string? id, Func<T> allowed, Func<T> blocked)
    {
        lock (gate)
        {
            CheckOpen();
            return id is not null && identities.TryGetValue(id, out var known) && !known.Unconfirmed ? blocked() : allowed();
        }
    }

    public ulong Generation(string id)
    {
        lock (gate) { CheckOpen(); return identities[id].Generation; }
    }

    public HostRoute? Resolve(string id)
    {
        lock (gate)
        {
            CheckOpen();
            if (!routes.TryGetValue(id, out var route)) return null;
            if (route.ExpiresAt > now()) return route;
            routes.Remove(id);
            return null;
        }
    }

    // A route-dependent adapter mutation must share the ownership gate. In particular a
    // delayed direct socket/ticket must not attach after the desktop has claimed the route.
    internal T Current<T>(string id, ulong generation, HostKind kind, Func<HostRoute,T> accepted, Func<T> rejected)
    {
        lock (gate) {
            CheckOpen(); var route = Resolve(id);
            return route is not null && route.Generation == generation && route.Kind == kind
                ? accepted(route) : rejected();
        }
    }
    internal byte[] DirectReplyProof(string id, ReadOnlySpan<byte> bytes)
    {
        lock (gate) {
            CheckOpen(); var key = Convert.FromBase64String(identities[id].Key);
            try {
                var domain = Encoding.ASCII.GetBytes("NXRP-DIRECT-REPLY-1");
                var input = new byte[domain.Length + bytes.Length];
                domain.CopyTo(input,0); bytes.CopyTo(input.AsSpan(domain.Length));
                return HMACSHA256.HashData(key,input);
            } finally { CryptographicOperations.ZeroMemory(key); }
        }
    }

    public OwnerChallenge? Challenge(string id, OwnerAction action, HostKind kind,
        string owner, string endpoint, ulong expectedGeneration)
    {
        ValidateId(id);
        ValidateText(owner, 128);
        ValidateText(endpoint, 256);
        if (!Enum.IsDefined(action) || !Enum.IsDefined(kind)) return null;
        lock (gate)
        {
            CheckOpen();
            PruneChallenges();
            if (!identities.TryGetValue(id, out var identity) ||
                identity.Generation != expectedGeneration || pendingIds.Contains(id) ||
                challenges.Count >= MaximumChallenges) return null;
            var challenge = new OwnerChallenge(id, action, kind, owner, endpoint,
                expectedGeneration, Convert.ToHexString(RandomNumberGenerator.GetBytes(32)),
                DateTimeOffset.FromUnixTimeMilliseconds((now() + ChallengeDuration).ToUnixTimeMilliseconds()));
            challenges.Add(challenge.Nonce, challenge);
            return challenge;
        }
    }

    // The exact canonical request is authenticated, including kind, endpoint, owner, operation
    // and expected generation. Length prefixes prevent delimiter ambiguities. Nonces are consumed
    // on failed authentication too, so a captured request is never a repeatable mutation.
    public static byte[] Proof(OwnerChallenge challenge, ReadOnlySpan<byte> key)
    {
        return HMACSHA256.HashData(key, Canonical(challenge));
    }
    internal byte[] NativeReplyProof(BridgeChallengeRequest scope,ulong generation,long expires,NativeOwnerReply reply)
    {
        lock(gate) {
            CheckOpen();var key=Convert.FromBase64String(identities[scope.Id].Key);
            try{return HMACSHA256.HashData(key,NativeOwnerAuthentication.Canonical(scope,generation,expires,reply));}
            finally{CryptographicOperations.ZeroMemory(key);}
        }
    }
    public static byte[] Canonical(OwnerChallenge challenge) => OwnerProofBytes.Serialize(challenge.Id,
        (byte)challenge.Action, (byte)challenge.Kind, challenge.Owner, challenge.Endpoint,
        challenge.Generation, challenge.Nonce, challenge.ExpiresAt.ToUnixTimeMilliseconds());

    public HostRoute? Authenticate(OwnerChallenge supplied, ReadOnlySpan<byte> proof)
    {
        lock (gate)
        {
            CheckOpen();
            if (!challenges.Remove(supplied.Nonce, out var challenge) || challenge != supplied ||
                challenge.ExpiresAt <= now() || proof.Length != 32 ||
                !identities.TryGetValue(challenge.Id, out var identity) ||
                identity.Generation != challenge.Generation || pendingIds.Contains(challenge.Id))
                return null;
            var key = Convert.FromBase64String(identity.Key);
            bool authenticated;
            try { authenticated = CryptographicOperations.FixedTimeEquals(Proof(challenge, key), proof); }
            finally { CryptographicOperations.ZeroMemory(key); }
            if (!authenticated) return null;
            if (identity.Unconfirmed)
            {
                // The holder of this key just proved it. From now on the key is locked in.
                identity = identity with { Unconfirmed = false };
                var confirmed = new Dictionary<string, Identity>(identities) { [challenge.Id] = identity };
                Save(confirmed);
                identities = confirmed;
            }

            var active = Resolve(challenge.Id);
            if (challenge.Action != OwnerAction.Claim)
            {
                if (active is null || active.Owner != challenge.Owner || active.Kind != challenge.Kind ||
                    active.Endpoint != challenge.Endpoint || active.Generation != challenge.Generation)
                    return null;
                if (challenge.Action == OwnerAction.Renew)
                {
                    var renewed = active with { ExpiresAt = now() + LeaseDuration };
                    routes[challenge.Id] = renewed;
                    return renewed;
                }
                Advance(challenge.Id, identity);
                routes.Remove(challenge.Id);
                // A release acknowledgement is an expired snapshot with the new generation.
                return active with { Generation = identities[challenge.Id].Generation, ExpiresAt = now() };
            }

            // A console can hand its direct route to its USB delegate; a live bridge cannot be
            // displaced by another desktop or an impatient direct reclaim. Release or expiry first.
            if (active?.Kind == HostKind.UsbBridge) return null;
            Advance(challenge.Id, identity);
            var claimed = new HostRoute(challenge.Id, challenge.Kind, challenge.Owner,
                challenge.Endpoint, identities[challenge.Id].Generation, now() + LeaseDuration);
            routes[challenge.Id] = claimed;
            return claimed;
        }
    }

    void Advance(string id, Identity identity)
    {
        var candidate = new Dictionary<string, Identity>(identities)
        { [id] = identity with { Generation = checked(identity.Generation + 1) } };
        // Fail closed on disk error: do not publish a generation that was not durably saved.
        pendingIds.Add(id);
        Save(candidate);
        identities = candidate;
        pendingIds.Remove(id);
        RemoveChallenges(id);
    }

    void Save(Dictionary<string, Identity> candidate)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        string temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            using (var file = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write,
                FileShare.None, 4096, FileOptions.WriteThrough))
            {
                JsonSerializer.Serialize(file, candidate);
                file.Flush(flushToDisk: true);
            }
            File.Move(temporary, path, overwrite: true);
        }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }

    void PruneChallenges()
    {
        foreach (var item in challenges.Where(x => x.Value.ExpiresAt <= now()).ToArray())
            challenges.Remove(item.Key);
    }
    void RemoveChallenges(string id)
    {
        foreach (var item in challenges.Where(x => x.Value.Id == id).ToArray())
            challenges.Remove(item.Key);
    }
    static void ValidateId(string id)
    {
        if (id.Length != 5 || id.Any(c => !"23456789ABCDEFGHJKLMNPQRSTUVWXYZ".Contains(c)))
            throw new ArgumentException("Invalid identity");
    }
    static void ValidateText(string value, int maximum)
    {
        if (string.IsNullOrWhiteSpace(value) || value.Length > maximum || value.Any(c => c < 33 || c > 126))
            throw new ArgumentException("Invalid ownership field");
    }
    void CheckOpen() { ObjectDisposedException.ThrowIf(disposed, this); }
    public void Dispose()
    {
        lock (gate)
        {
            disposed = true;
            routes.Clear();
            challenges.Clear();
            identities.Clear();
            stateLock.Dispose();
        }
    }
}
