using NXRemotePlay.Protocol;
using System;
using System.Threading;
using System.Threading.Tasks;
namespace NxRemotePlay.Client.Bridge;
internal interface IBridgeOwnerClient : IDisposable
{
    string ServerHost { get; }
    Task<BridgeChallengeReply> Challenge(BridgeChallengeRequest request, CancellationToken cancel);
    Task<BridgeRouteReply> Apply(BridgeProofRequest request, CancellationToken cancel);
}
