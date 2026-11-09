using NXRemotePlay.Protocol;
using System.Buffers.Binary;
using System.Net.Sockets;
using System.Text.Json;

namespace Rendezvous;

// Desktop-only ownership extension on the existing TCP rendezvous listener.
// Existing PUNCHJOIN/PUNCHHOST and native media protocols are unchanged.
internal static class NativeOwnerService
{
    internal static async Task Handle(NetworkStream stream,HostOwnership? ownership,int port,CancellationToken cancel)
    {
        var header=new byte[4];await stream.ReadExactlyAsync(header,cancel);
        int size=BinaryPrimitives.ReadInt32BigEndian(header);if(size is <1 or >4096)throw new IOException("Invalid owner frame size.");
        var bytes=new byte[size];await stream.ReadExactlyAsync(bytes,cancel);
        var request=JsonSerializer.Deserialize(bytes,BridgeJson.Default.NativeOwnerRequest)??throw new ArgumentException("Missing owner request.");
        NativeOwnerReply reply;
        if(ownership is null)reply=new(Error:"OWNERSHIP_NOT_CONFIGURED");
        else if(request.Operation=="challenge"&&request.Challenge is { } scope&&request.Proof is null) {
            Validate(scope);
            if(!ownership.IsProtected(scope.Id))reply=new(Error:"IDENTITY_NOT_ENROLLED");
            else {
                ulong generation=scope.Generation==0&&scope.Action==0?ownership.Generation(scope.Id):scope.Generation;
                var challenge=ownership.Challenge(scope.Id,(OwnerAction)scope.Action,HostKind.UsbBridge,scope.Owner,scope.Endpoint,generation);
                if(challenge is null)reply=new(Error:"OWNER_CONFLICT");
                else {
                    var granted=scope with {Generation=generation};
                    reply=new(Challenge:new(granted,challenge.Nonce,challenge.ExpiresAt.ToUnixTimeMilliseconds(),HostOwnership.Canonical(challenge)));
                    reply=reply with {Authentication=ownership.NativeReplyProof(granted,generation,challenge.ExpiresAt.ToUnixTimeMilliseconds(),reply)};
                }
            }
        } else if(request.Operation=="proof"&&request.Proof is { } supplied&&request.Challenge is null) {
            var proofScope=supplied.Request;Validate(proofScope);
            var challenge=new OwnerChallenge(proofScope.Id,(OwnerAction)proofScope.Action,HostKind.UsbBridge,proofScope.Owner,proofScope.Endpoint,proofScope.Generation,supplied.Nonce,DateTimeOffset.FromUnixTimeMilliseconds(supplied.ExpiresAt));
            var route=ownership.Authenticate(challenge,supplied.Proof);
            if(route is null)reply=new(Error:"OWNER_AUTHENTICATION_FAILED");
            else {
                reply=new(Route:new(route.Id,(byte)route.Kind,route.Owner,route.Endpoint,route.Generation,route.ExpiresAt.ToUnixTimeMilliseconds(),PunchTicket:proofScope.Action==2?null:ownership.PunchTickets.Issue(route),PunchPort:port));
                // Keep the signed generation at the request generation: USB commit
                // expects the server's claimed route to advance that generation.
                reply=reply with {Authentication=ownership.NativeReplyProof(proofScope,proofScope.Generation,supplied.ExpiresAt,reply)};
            }
        } else throw new ArgumentException("Invalid owner operation.");
        bytes=JsonSerializer.SerializeToUtf8Bytes(reply,BridgeJson.Default.NativeOwnerReply);
        if(bytes.Length>4096)throw new IOException("Owner reply too large.");
        BinaryPrimitives.WriteInt32BigEndian(header,bytes.Length);await stream.WriteAsync(header,cancel);await stream.WriteAsync(bytes,cancel);
    }
    static void Validate(BridgeChallengeRequest scope)
    {
        if(scope is null||scope.Action>2||scope.Kind!=1)throw new ArgumentException("Invalid owner scope.");
        _=NativeBridgeRoute.Parse(scope.Endpoint);
    }
}
