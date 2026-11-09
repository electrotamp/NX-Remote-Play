using System;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace NxRemotePlay.Client.Bridge;

// Responds to the same discovery datagram as the Switch. No receiving client changes.
internal sealed class NativeLanDiscovery : IAsyncDisposable
{
    readonly UdpClient socket;
    readonly CancellationTokenSource cancel;
    readonly Task reading;

    internal NativeLanDiscovery(string id, NativeForwarder native, Func<bool> online, CancellationToken lifetime, int port = 9913)
    {
        socket = new UdpClient(new IPEndPoint(IPAddress.Any, port));
        // a client that already closed its discovery socket answers our reply with ICMP "port unreachable";
        // on Windows that made the next ReceiveAsync throw 10054 and silently ended this responder.
        NxRemotePlay.Client.Sources.PunchSource.IgnoreUdpUnreachable(socket);
        cancel = CancellationTokenSource.CreateLinkedTokenSource(lifetime);
        reading = Task.Run(async () =>
        {
            try
            {
                while (!cancel.IsCancellationRequested)
                {
                    try
                    {
                        var packet = await socket.ReceiveAsync(cancel.Token);
                        if (packet.Buffer.Length > 64 || !online() || Encoding.ASCII.GetString(packet.Buffer).Trim() != $"NXLAN1 DISCOVER {id}") continue;
                        string reply = native.HasRoom ? $"NXLAN1 HERE {id} {native.Endpoint.Port}\n" : $"NXLAN1 BUSY {id}\n";
                        await socket.SendAsync(Encoding.ASCII.GetBytes(reply), packet.RemoteEndPoint, cancel.Token);
                    }
                    catch (SocketException) { }
                }
            }
            catch (OperationCanceledException) when (cancel.IsCancellationRequested) { }
        });
    }
    public async ValueTask DisposeAsync()
    {
        cancel.Cancel();
        try { await reading; } finally { socket.Dispose(); cancel.Dispose(); }
    }
}
