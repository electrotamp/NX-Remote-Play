using NxRemotePlay.Client.Sources;
using System.Threading;
using System.Threading.Tasks;
namespace NxRemotePlay.Client.Bridge;
// Shared USB ownership/input contract; media still comes from one StreamManager.
internal interface IBridgeHostSource : IInputSink
{
    string SessionId { get; }
    string SessionPassword { get; }
    bool SupportsOwnerProof { get; }
    bool SupportsDelegation { get; }
    Task<byte[]> RequestOwnerProof(byte[] canonical, CancellationToken cancel);
    Task RequestDelegation(byte operation, ulong generation, uint milliseconds, CancellationToken cancel);
}
