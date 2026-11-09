using System.Threading.Tasks;
namespace NxRemotePlay.Client.Sources;
internal interface IInputSink
{
    bool SupportsInput { get; }
    Task SendInput(byte[] packet);
}
