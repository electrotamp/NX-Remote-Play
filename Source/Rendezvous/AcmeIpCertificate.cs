using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;
using Microsoft.Extensions.Logging;
using System.Net;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace Rendezvous;

// RFC 8555 + RFC 8738 HTTP-01. This runs only after the user enables the
// certificate feature and accepts the CA agreement in the desktop settings.
public static class AcmeIpCertificate
{
    public const string AgreementUrl="https://letsencrypt.org/documents/LE-SA-v1.8-July-06-2026.pdf";
    static readonly SemaphoreSlim issuing=new(1);
    static string Folder=>Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),"NX-Remote-Play","PublicCertificates");
    public static string CertificatePath(IPAddress address)=>Path.Combine(Folder,address+".pfx");
    public static bool NeedsRenewal(IPAddress address)
    {
        string path=CertificatePath(address);
        if(!File.Exists(path)) return true;
        using var cert=X509CertificateLoader.LoadPkcs12FromFile(path,null);
        using var chain=new X509Chain();
        return !cert.HasPrivateKey||!cert.MatchesHostname(address.ToString(),false,false)||
            cert.NotAfter.ToUniversalTime()<DateTime.UtcNow.AddDays(2)||!chain.Build(cert);
    }
    public static async Task<string> Ensure(IPAddress address,bool agreementAccepted,CancellationToken cancel)
    {
        if(!agreementAccepted) throw new InvalidOperationException("Accept the Let's Encrypt subscriber agreement before requesting a certificate.");
        if(!AddressDetection.IsPublic(address)) throw new ArgumentException("Certificate issuance requires a public IPv4 address.");
        await issuing.WaitAsync(cancel);
        try {
            if(!NeedsRenewal(address)) return CertificatePath(address);
            Directory.CreateDirectory(Folder);
            using var key=ECDsa.Create(ECCurve.NamedCurves.nistP256);
            string keyPath=Path.Combine(Folder,"account.pk8");
            if(File.Exists(keyPath)) {
                byte[] stored=File.ReadAllBytes(keyPath);
                try {key.ImportPkcs8PrivateKey(stored,out _);} finally {CryptographicOperations.ZeroMemory(stored);}
            } else {
                byte[] stored=key.ExportPkcs8PrivateKey();
                try {File.WriteAllBytes(keyPath,stored);} finally {CryptographicOperations.ZeroMemory(stored);}
            }
            using var timeout=CancellationTokenSource.CreateLinkedTokenSource(cancel);timeout.CancelAfter(TimeSpan.FromMinutes(3));
            using var http=new HttpClient(new HttpClientHandler {AllowAutoRedirect=false}) {Timeout=TimeSpan.FromSeconds(15),MaxResponseContentBufferSize=1024*1024};
            using var directory=JsonDocument.Parse(await http.GetStringAsync("https://acme-v02.api.letsencrypt.org/directory",timeout.Token));
            var root=directory.RootElement;
            if(root.GetProperty("meta").GetProperty("termsOfService").GetString()!=AgreementUrl) throw new IOException("The certificate subscriber agreement has changed. Review the new agreement before issuance.");
            if(!root.GetProperty("meta").GetProperty("profiles").TryGetProperty("shortlived",out _)) throw new IOException("The certificate provider does not support public-IP certificates.");
            var session=new Session(http,key,new Uri(root.GetProperty("newNonce").GetString()!));
            using var account=await session.Post(root.GetProperty("newAccount").GetString()!,new {termsOfServiceAgreed=true},timeout.Token);
            session.Account=account.Headers.Location?.ToString()??throw new IOException("Certificate account response missing.");
            using var order=await session.Post(root.GetProperty("newOrder").GetString()!,new {identifiers=new[]{new {type="ip",value=address.ToString()}},profile="shortlived"},timeout.Token);
            string orderUrl=order.Headers.Location?.ToString()??throw new IOException("Certificate order response missing.");
            using var orderJson=JsonDocument.Parse(await order.Content.ReadAsStringAsync(timeout.Token));
            string finalize=orderJson.RootElement.GetProperty("finalize").GetString()!;
            foreach(var authUrl in orderJson.RootElement.GetProperty("authorizations").EnumerateArray()) {
                using var authorization=await session.Post(authUrl.GetString()!,null,timeout.Token);
                using var auth=JsonDocument.Parse(await authorization.Content.ReadAsStringAsync(timeout.Token));
                if(auth.RootElement.GetProperty("identifier").GetProperty("type").GetString()!="ip"||
                    auth.RootElement.GetProperty("identifier").GetProperty("value").GetString()!=address.ToString()) throw new IOException("Certificate authorization changed the requested address.");
                if(auth.RootElement.GetProperty("status").GetString()=="valid") continue;
                var challenge=auth.RootElement.GetProperty("challenges").EnumerateArray().First(c=>c.GetProperty("type").GetString()=="http-01");
                string token=challenge.GetProperty("token").GetString()!;
                if(!Regex.IsMatch(token,@"^[A-Za-z0-9_-]{16,256}$")) throw new IOException("Invalid certificate challenge token.");
                string answer=token+"."+Thumbprint(key);
                var builder=WebApplication.CreateSlimBuilder();builder.Logging.ClearProviders();
                builder.WebHost.ConfigureKestrel(options=>options.Listen(IPAddress.Any,80));
                await using var proof=builder.Build();
                proof.MapGet("/.well-known/acme-challenge/"+token,async context=> {
                    if(context.Request.Host.Host!=address.ToString()) {context.Response.StatusCode=404;return;}
                    context.Response.ContentType="text/plain";await context.Response.WriteAsync(answer,context.RequestAborted);
                });
                await proof.StartAsync(timeout.Token);
                try {
                    using var acknowledged=await session.Post(challenge.GetProperty("url").GetString()!,new {},timeout.Token);
                    using var validated=await session.Poll(authUrl.GetString()!,"valid",timeout.Token);
                } finally {await proof.StopAsync(CancellationToken.None);}
            }
            using var rsa=RSA.Create(2048);
            var request=new CertificateRequest(new X500DistinguishedName(new byte[]{0x30,0x00}),rsa,HashAlgorithmName.SHA256,RSASignaturePadding.Pkcs1);
            var san=new SubjectAlternativeNameBuilder();san.AddIpAddress(address);request.CertificateExtensions.Add(san.Build());
            using var ready=await session.Poll(orderUrl,"ready",timeout.Token);
            using var finalized=await session.Post(finalize,new {csr=Base64(request.CreateSigningRequest())},timeout.Token);
            using var valid=await session.Poll(orderUrl,"valid",timeout.Token);
            using var download=await session.Post(valid.RootElement.GetProperty("certificate").GetString()!,null,timeout.Token);
            var collection=new X509Certificate2Collection();collection.ImportFromPem(await download.Content.ReadAsStringAsync(timeout.Token));
            try {
                if(collection.Count==0) throw new IOException("Certificate provider returned an empty chain.");
                using var leaf=collection[0].CopyWithPrivateKey(rsa);
                if(!leaf.MatchesHostname(address.ToString(),false,false)) throw new IOException("Issued certificate does not cover the public address.");
                using var chain=new X509Chain();chain.ChainPolicy.ExtraStore.AddRange(collection);
                chain.ChainPolicy.ApplicationPolicy.Add(new Oid("1.3.6.1.5.5.7.3.1"));
                if(!chain.Build(leaf)) throw new IOException("Issued certificate is not trusted by Windows.");
                var output=new X509Certificate2Collection(leaf);for(int i=1;i<collection.Count;i++) output.Add(collection[i]);
                byte[] pfx=output.Export(X509ContentType.Pkcs12)!;
                string path=CertificatePath(address),temporary=path+".new";
                try {File.WriteAllBytes(temporary,pfx);File.Move(temporary,path,true);} finally {CryptographicOperations.ZeroMemory(pfx);}
                return path;
            } finally {foreach(var cert in collection) cert.Dispose();}
        } finally {issuing.Release();}
    }
    public static string Base64(ReadOnlySpan<byte> bytes)=>Convert.ToBase64String(bytes).TrimEnd('=').Replace('+','-').Replace('/','_');
    public static string Thumbprint(ECDsa key)
    {
        var p=key.ExportParameters(false);
        string jwk=JsonSerializer.Serialize(new {crv="P-256",kty="EC",x=Base64(p.Q.X!),y=Base64(p.Q.Y!)});
        return Base64(SHA256.HashData(Encoding.UTF8.GetBytes(jwk)));
    }
    internal sealed class Session(HttpClient http,ECDsa key,Uri nonceUrl)
    {
        public string? Account;
        string? nonce;
        void Validate(string url) {var parsed=new Uri(url);if(nonceUrl.Scheme!="https"||nonceUrl.Port!=443||nonceUrl.UserInfo.Length!=0||parsed.Scheme!="https"||parsed.Host!=nonceUrl.Host||parsed.Port!=443||parsed.UserInfo.Length!=0) throw new IOException("Unexpected certificate provider URL.");}
        public async Task<HttpResponseMessage> Post(string url,object? payload,CancellationToken cancel)
        {
            Validate(url);
            for(int attempt=0;attempt<2;attempt++) {
                if(nonce is null) {
                    using var head=await http.SendAsync(new HttpRequestMessage(HttpMethod.Head,nonceUrl),cancel);head.EnsureSuccessStatusCode();
                    nonce=head.Headers.GetValues("Replay-Nonce").Single();
                }
                var p=key.ExportParameters(false);
                var headers=new Dictionary<string,object> {{"alg","ES256"},{"nonce",nonce},{"url",url}};
                if(Account is null) headers["jwk"]=new {crv="P-256",kty="EC",x=Base64(p.Q.X!),y=Base64(p.Q.Y!)};else headers["kid"]=Account;
                string protectedValue=Base64(JsonSerializer.SerializeToUtf8Bytes(headers));
                string content=payload is null?"":Base64(JsonSerializer.SerializeToUtf8Bytes(payload));
                byte[] signature=key.SignData(Encoding.ASCII.GetBytes(protectedValue+"."+content),HashAlgorithmName.SHA256,DSASignatureFormat.IeeeP1363FixedFieldConcatenation);
                using var body=new StringContent(JsonSerializer.Serialize(new { @protected=protectedValue,payload=content,signature=Base64(signature)}),Encoding.UTF8,"application/jose+json");
                var response=await http.PostAsync(url,body,cancel);
                nonce=response.Headers.TryGetValues("Replay-Nonce",out var values)?values.Single():null;
                if(response.IsSuccessStatusCode) return response;
                string failure=await response.Content.ReadAsStringAsync(cancel);response.Dispose();
                using var problem=JsonDocument.Parse(failure);
                if(attempt==0&&problem.RootElement.TryGetProperty("type",out var type)&&type.GetString()=="urn:ietf:params:acme:error:badNonce") continue;
                throw new IOException("Certificate request rejected. Check public port 80 reachability and the provider's issuance limits.");
            }
            throw new IOException("Certificate nonce retry failed.");
        }
        public async Task<JsonDocument> Poll(string url,string expected,CancellationToken cancel)
        {
            for(int at=0;at<40;at++) {
                using var response=await Post(url,null,cancel);
                var state=JsonDocument.Parse(await response.Content.ReadAsStringAsync(cancel));
                string? status=state.RootElement.GetProperty("status").GetString();
                if(status==expected) return state;
                state.Dispose();
                if(status is "invalid" or "expired" or "revoked" or "deactivated") throw new IOException("Public-IP validation failed. The certificate provider must reach this PC on public TCP port 80.");
                await Task.Delay(2000,cancel);
            }
            throw new IOException("Public-IP certificate validation timed out.");
        }
    }
}
