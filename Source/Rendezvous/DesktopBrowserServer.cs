using NXRemotePlay.Protocol;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;

namespace Rendezvous;

// A desktop-local browser session, never a claim against the public rendezvous.
// Its ephemeral key belongs only to this local service, not to the Switch.
public sealed class DesktopBrowserServer : IAsyncDisposable
{
    readonly string directory;
    readonly byte[] key = RandomNumberGenerator.GetBytes(32);
    readonly string linkToken = Convert.ToHexString(RandomNumberGenerator.GetBytes(32));   // the session link 
    readonly CancellationTokenSource cancel = new();
    readonly HostOwnership owners;
    readonly BridgeSignalingServer server;
    readonly UdpClient stun;
    X509Certificate2 certificate;
    readonly List<X509Certificate2> previousCertificates=new();
    readonly string id, endpoint, owner = "desktop-browser-" + Guid.NewGuid().ToString("N");
    Task? renewing, discovery;
    HostRoute? route;
    public Uri Address { get; private set; } = null!;
    public Uri JoinLink { get; private set; } = null!;
    public string CertificatePin => Convert.ToHexString(certificate.GetCertHash(HashAlgorithmName.SHA256));
    public bool Active => renewing is { IsCompleted: false } && !cancel.IsCancellationRequested;
    public RtcIceServer[] IceServers { get; private set; } = [];

    public DesktopBrowserServer(string id, string password, IPAddress address, int port,
        IPAddress? publicAddress=null,string[]? discoveryUrls=null,string? trustedCertificatePath=null)
    {
        // Validate before allocating listeners or persistent files.
        _ = BrowserJoinLink.Create(address, port, id);
        this.id = id;
        endpoint = new NativeBridgeRoute(new IPEndPoint(IPAddress.Loopback, 1),
            NativeBridgeRoute.HashPassword(id, password)).ToString();
        directory = Path.Combine(Path.GetTempPath(), "nxrp-local-browser-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        try {
            owners = new HostOwnership(Path.Combine(directory, "owners.json"));
            owners.Provision(id, key);
            certificate = LoadCertificate(address,publicAddress,trustedCertificatePath);
            stun = new UdpClient(new IPEndPoint(IPAddress.Any, 0));
            int discoveryPort = ((IPEndPoint)stun.Client.LocalEndPoint!).Port;
            string localDiscovery="stun:"+address+":"+discoveryPort;
            string[] combined=discoveryUrls is {Length:>0}?discoveryUrls.Append(localDiscovery).Distinct().ToArray():[localDiscovery];
            server = new BridgeSignalingServer(owners, IPAddress.Any, port, certificate, directPort: discoveryPort,discoveryUrls:combined,certificateProvider:()=>Volatile.Read(ref certificate));
            IceServers = combined.Select(url=>new RtcIceServer([url])).ToArray();
            Address = new UriBuilder("https", address.ToString(), port).Uri;
            JoinLink = BrowserJoinLink.Create(address, port, id);
        } catch {
            stun?.Dispose(); certificate?.Dispose(); owners?.Dispose();
            CryptographicOperations.ZeroMemory(key);
            Directory.Delete(directory, true);
            throw;
        }
    }
    static X509Certificate2 LoadCertificate(IPAddress address,IPAddress? publicAddress,string? trustedPath)
    {
        if(!string.IsNullOrWhiteSpace(trustedPath)) {
            var trusted=X509CertificateLoader.LoadPkcs12FromFile(trustedPath,null);
            using var chain=new X509Chain();
            chain.ChainPolicy.ApplicationPolicy.Add(new Oid("1.3.6.1.5.5.7.3.1"));
            if(!trusted.HasPrivateKey||!chain.Build(trusted)||publicAddress is not null&&!trusted.MatchesHostname(publicAddress.ToString(),allowWildcards:false,allowCommonName:false)) {
                trusted.Dispose();throw new IOException("The HTTPS certificate must be trusted, contain a private key, and cover the public IP address.");
            }
            return trusted;
        }
        string path = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "NX-Remote-Play", "BrowserCertificates", address.ToString().Replace(':', '_') + (publicAddress is null?"":"_"+publicAddress) + ".pfx");
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        if (File.Exists(path)) {
            var existing = X509CertificateLoader.LoadPkcs12FromFile(path, null);
            if (existing.NotAfter.ToUniversalTime() > DateTime.UtcNow.AddDays(1)) return existing;
            existing.Dispose();
        }
        using var rsa = RSA.Create(2048);
        var request = new CertificateRequest("CN=NX Remote Play desktop", rsa, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
        var names = new SubjectAlternativeNameBuilder();
        names.AddIpAddress(address); names.AddIpAddress(IPAddress.Loopback); names.AddDnsName("localhost");
        if(publicAddress is not null&&!publicAddress.Equals(address)) names.AddIpAddress(publicAddress);
        request.CertificateExtensions.Add(names.Build());
        using var generated = request.CreateSelfSigned(DateTimeOffset.UtcNow.AddMinutes(-5), DateTimeOffset.UtcNow.AddDays(90));
        byte[] pfx = generated.Export(X509ContentType.Pfx);
        try { File.WriteAllBytes(path, pfx); return X509CertificateLoader.LoadPkcs12(pfx, null); }
        finally { CryptographicOperations.ZeroMemory(pfx); }
    }
    public void ReloadTrustedCertificate(string path,IPAddress publicAddress)
    {
        var replacement=LoadCertificate(IPAddress.Parse(Address.Host),publicAddress,path);
        previousCertificates.Add(Interlocked.Exchange(ref certificate,replacement));
    }
    HostRoute Apply(OwnerAction action)
    {
        var challenge = owners.Challenge(id, action, HostKind.UsbBridge, owner, endpoint, owners.Generation(id))
            ?? throw new IOException("Local browser session expired.");
        byte[] proof = HostOwnership.Proof(challenge, key);
        try { return owners.Authenticate(challenge, proof) ?? throw new IOException("Local browser session rejected."); }
        finally { CryptographicOperations.ZeroMemory(proof); }
    }
    public async Task Start(CancellationToken lifetime)
    {
        lifetime.ThrowIfCancellationRequested();
        await server.Start(lifetime);
        route = Apply(OwnerAction.Claim);
        if(!server.RegisterSessionLink(route,linkToken)) throw new IOException("Could not register the session link.");
        renewing = Task.Run(async () => {
            try {
                while (!cancel.IsCancellationRequested) {
                    await Task.Delay(4000, cancel.Token);
                    route = Apply(OwnerAction.Renew);
                }
            } catch (OperationCanceledException) when (cancel.IsCancellationRequested) { }
        });
        discovery = Task.Run(async () => {
            try {
                while (!cancel.IsCancellationRequested) {
                    var packet = await stun.ReceiveAsync(cancel.Token);
                    var reply = StunBinding.Reply(packet.Buffer, packet.RemoteEndPoint);
                    if (reply is not null) await stun.SendAsync(reply, packet.RemoteEndPoint, cancel.Token);
                }
            } catch (OperationCanceledException) when (cancel.IsCancellationRequested) { }
        });
    }
    // The join token of this browser session: the same value every time, good for any number of viewers (up to the viewer limit) until the
    // session ends (USB unplugged, app closed). Never contains the password.
    public string SessionLinkToken => linkToken;
    public string HostTicket() => server.LocalHostTicket(owners.Resolve(id)
        ?? throw new IOException("Local browser session expired."));
    public async ValueTask DisposeAsync()
    {
        cancel.Cancel();
        try { if (renewing is not null) await renewing; }
        catch (Exception) { }
        try { if (discovery is not null) await discovery; }
        catch (Exception) { }
        try { if (route is not null) Apply(OwnerAction.Release); }
        finally {
            try { await server.DisposeAsync(); }
            finally {
                stun.Dispose(); owners.Dispose(); certificate.Dispose(); cancel.Dispose();
                foreach(var previous in previousCertificates) previous.Dispose();
                CryptographicOperations.ZeroMemory(key);
                Directory.Delete(directory, true);
            }
        }
    }
}
