using System.Collections.Concurrent;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using NXRemotePlay.Protocol;

namespace Rendezvous;

// One registered host and, once claimed, the connector paired against it.
class RendezvousSession
{
	public required string Id { get; init; }
	public required string Password { get; init; }
	public required IPEndPoint HostEndpoint { get; set; }
	public DateTime LastSeenUtc { get; set; }

	//  (TCP transport): set by PUNCHWAKE, consumed by the host's next REFRESH reply
	// ("WAKE" instead of "OK"). This is how a connector asks a sleeping-on-the-menu console to
	// start a punch attempt without the console holding any connection open.
	public volatile bool WakePending;
	public ulong OwnerGeneration { get; set; }

	//  (TCP transport): the host's open TCP signaling connection while it is waiting for a
	// connector (PUNCHHOST). PEER is pushed down it by PUNCHJOIN.
	public TcpClient? HostTcp { get; set; }
	public NetworkStream? HostTcpStream { get; set; }
	public IPEndPoint? HostTcpObserved { get; set; }

	// the host registered with PUNCHHOST2, i.e. it understands the connection ticket
	// (see HandlePunchJoin). Set on every attach so a stale value from a previous host never sticks.
	public bool HostTicketCapable { get; set; }
}

// Session ID/Password registration, refresh, UDP endpoint exchange and TCP hole-punch
// coordination. TCP coordination and wake messages:
public class RendezvousServer
{
    readonly HostOwnership? _ownership;
    readonly DirectOwnerWire? _direct;
    public RendezvousServer(HostOwnership? ownership = null) {
        _ownership = ownership; _direct = ownership is null ? null : new DirectOwnerWire(ownership);
    }
	// Excludes ambiguous characters (0/1/I/O).
	const string CredentialAlphabet = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
	const int CredentialLength = 5;

	// Abandoned registrations expire after this interval. Hosts must refresh well before
	// expiry; a refresh every two minutes or less leaves margin under the five-minute TTL.
	static readonly TimeSpan SessionTtl = TimeSpan.FromMinutes(5);
	static readonly TimeSpan SweepInterval = TimeSpan.FromSeconds(30);

	// Hard ceiling on total concurrent sessions, independent of rate limiting.
	const int MaxConcurrentSessions = 10_000;

	const int MaxTcpLineLength = 160;

	// REGISTER: flood protection. CONNECT/PUNCH*: brute-forcing a password against a known
	// Session ID is only ~33 million combinations, so this one matters for security.
	readonly RateLimiter _registerLimiter = new(maxPerWindow: 5, window: TimeSpan.FromSeconds(10));
	readonly RateLimiter _connectLimiter = new(maxPerWindow: 20, window: TimeSpan.FromSeconds(10));
	readonly RateLimiter _refreshLimiter = new(maxPerWindow: 10, window: TimeSpan.FromSeconds(10));
	// liveness probes ("REFRESH __PROBE__ __PROBE__" from the phone app, "__reachability_probe__"
	// from the NRO footer) used to share _refreshLimiter with real host REFRESH/UNREGISTER. Everything at one
	// home shares one public IP, so a few probing clients exhausted the 10-per-10-s budget and the server then
	// silently dropped genuine host keepalives/wakes. Probes now have their own bucket and never touch the
	// real limiter or the log.
	readonly RateLimiter _probeLimiter = new(maxPerWindow: 60, window: TimeSpan.FromSeconds(10));
	readonly RateLimiter _stunLimiter = new(maxPerWindow: 120, window: TimeSpan.FromSeconds(10));
    readonly RateLimiter _ownerLimiter = new(maxPerWindow:60,window:TimeSpan.FromSeconds(10));
	// First-use owner-key enrollment: a caller can only guess a live password this fast, per source address.
	internal RateLimiter EnrollLimiter { get; set; } = new(maxPerWindow: 6, window: TimeSpan.FromSeconds(60));
	static bool IsProbeId(string id) => id == "__PROBE__" || id == "__reachability_probe__";

	readonly ConcurrentDictionary<string, RendezvousSession> _sessions = new();

	UdpClient? _socket;
	TcpListener? _tcpListener;

	public event Action<string>? Log;

	public async Task RunAsync(int port, CancellationToken cancel)
	{
		_socket = new UdpClient(port);
		using var udpLifetime = _socket;
		Log?.Invoke($"Rendezvous listening on UDP {port}");

		_tcpListener = new TcpListener(IPAddress.Any, port);
		_tcpListener.Start();
		Log?.Invoke($"Rendezvous listening on TCP {port} (punch signaling)");

		_ = SweepLoopAsync(cancel);
		_ = TcpAcceptLoopAsync(_tcpListener, cancel);

		while (!cancel.IsCancellationRequested)
		{
			UdpReceiveResult result;
			try
			{
				result = await _socket.ReceiveAsync(cancel).ConfigureAwait(false);
			}
			catch (OperationCanceledException)
			{
				break;
			}
			catch (Exception ex)
			{
				// e.g. WSAECONNRESET from an ICMP "port unreachable" after a Send to a vanished host
				Log?.Invoke($"UDP receive failed: {ex.Message}");
				continue;
			}

			try
			{
				HandleDatagram(result.Buffer, result.RemoteEndPoint);
			}
			catch (Exception ex)
			{
				Log?.Invoke($"UDP handling failed: {ex.Message}");
			}
		}

		_tcpListener.Stop();
	}

	// Expires abandoned sessions and trims idle rate-limit buckets.
	async Task SweepLoopAsync(CancellationToken cancel)
	{
		while (!cancel.IsCancellationRequested)
		{
			try
			{
				await Task.Delay(SweepInterval, cancel).ConfigureAwait(false);
			}
			catch (OperationCanceledException)
			{
				break;
			}

			var cutoff = DateTime.UtcNow - SessionTtl;
			int removed = 0;
			foreach (var (id, session) in _sessions)
			{
				if (session.LastSeenUtc >= cutoff || !_sessions.TryRemove(id, out _))
					continue;

				var idleFor = DateTime.UtcNow - session.LastSeenUtc;
				Log?.Invoke($"Session {id} expired - host {session.HostEndpoint} went silent ({idleFor.TotalSeconds:F0}s, no REFRESH)");
				session.HostTcp?.Dispose();
				removed++;
			}

			if (removed > 0)
				Log?.Invoke($"{removed} session(s) expired this sweep, {_sessions.Count} still active");

			_registerLimiter.SweepIdle(SessionTtl);
			_connectLimiter.SweepIdle(SessionTtl);
			_refreshLimiter.SweepIdle(SessionTtl);
			_probeLimiter.SweepIdle(SessionTtl);
			_stunLimiter.SweepIdle(SessionTtl);
            _ownerLimiter.SweepIdle(SessionTtl);
			EnrollLimiter.SweepIdle(SessionTtl);
		}
	}

	readonly UdpMediaRendezvous _udpMedia = new();
	void HandleDatagram(byte[] buffer, IPEndPoint from)
	{
        if (buffer.Length >= 4 && System.Buffers.Binary.BinaryPrimitives.ReadUInt32LittleEndian(buffer) == UdpMediaRendezvous.Magic) {
            _udpMedia.Log ??= line => Log?.Invoke(line);
            if (!_stunLimiter.Allow(from.Address)) Log?.Invoke($"NUP1 datagram from {from} rate limited");
            else foreach (var reply in _udpMedia.Handle(buffer, from, Environment.TickCount64)) _socket!.Send(reply.Data, reply.To);
            return;
        }
		if(StunBinding.IsStun(buffer)) {
			if(_stunLimiter.Allow(from.Address)) {var reply=StunBinding.Reply(buffer,from);if(reply is not null) _socket!.Send(reply,from);}
			return;
		}
		if (buffer.Length >= 4 && System.Buffers.Binary.BinaryPrimitives.ReadUInt32LittleEndian(buffer) == DirectOwnerWire.Magic) {
			var reply = _direct?.Handle(buffer,from);
			if (reply is not null) _socket!.Send(reply,from);
			return;
		}
		if (_ownership is null)
		{
			HandleLegacyDatagram(buffer, from);
			return;
		}
		if (buffer.Length > 64) return;
		var parts = Encoding.UTF8.GetString(buffer).TrimEnd('\0', '\r', '\n')
			.Split(' ', StringSplitOptions.RemoveEmptyEntries);
		var id = parts.Length == 3 ? parts[1] : null;
		if (parts.Length == 3 && parts[0] == "PUNCHWAKE" && id is not null && _ownership.IsFenced(id)) {
			if (_connectLimiter.Allow(from.Address)) {
                var route=_ownership.Resolve(id);
                bool bridgeReady=route is not null&&route.Kind==HostKind.UsbBridge&&AcceptsBridge(route,id,parts[2]);
                Send(from,bridgeReady||_direct!.Wake(id,parts[2]) ? "OK" : "ERR HOSTUNREACHABLE");
            }
			return;
		}
		_ownership.Legacy(id, () => { HandleLegacyDatagram(buffer, from); return true; }, () =>
		{
			// Until the authenticated route adapter exists, fail closed for joins too. A stale
			// direct socket must never be advertised as the USB bridge's current transport.
			Send(from, parts[0] is "CONNECT" or "PUNCHWAKE" ? "ERR HOSTUNREACHABLE" : "ERR OWNERAUTH");
			return false;
		});
	}

	void HandleLegacyDatagram(byte[] buffer, IPEndPoint from)
	{
		// Every real message here is well under 64 bytes.
		if (buffer.Length > 64)
			return;

		string text = Encoding.UTF8.GetString(buffer).TrimEnd('\0', '\r', '\n');
		var parts = text.Split(' ', StringSplitOptions.RemoveEmptyEntries);
		if (parts.Length == 0)
			return;

		switch (parts[0])
		{
			case "REGISTER" when parts.Length == 1:
				if (!_registerLimiter.Allow(from.Address))
				{
					Log?.Invoke($"REGISTER from {from} - rate limited");
					return; // silently drop - don't distinguish "rate limited" from other failures
				}

				HandleRegister(from);
				break;

			// A host resuming a previously-issued id+password (persisted on its own SD card).
			case "REGISTER" when parts.Length == 3:
				if (!_registerLimiter.Allow(from.Address))
				{
					Log?.Invoke($"REGISTER (reclaim) from {from} - rate limited");
					return;
				}

				HandleRegisterReclaim(parts[1], parts[2], from);
				break;

			case "CONNECT" when parts.Length == 3:
				if (!_connectLimiter.Allow(from.Address))
				{
					Log?.Invoke($"CONNECT from {from} - rate limited");
					return;
				}

				HandleConnect(parts[1], parts[2], from);
				break;

			// Host keepalive. Does NOT mint a new code. Replies "WAKE" instead of "OK" if a
			// connector has flagged a pending wake (PUNCHWAKE).
			case "REFRESH" when parts.Length == 3:
				if (IsProbeId(parts[1]))
				{
					// Answer exactly as an unknown id would (clients only check that a reply arrives).
					if (_probeLimiter.Allow(from.Address))
						Send(from, "ERR NOTFOUND");
					break;
				}

				if (!_refreshLimiter.Allow(from.Address))
				{
					Log?.Invoke($"REFRESH from {from} - rate limited");
					return;
				}

				HandleRefresh(parts[1], parts[2], from);
				break;

			// Connector asks the console to start a punch attempt.
			case "PUNCHWAKE" when parts.Length == 3:
				if (!_connectLimiter.Allow(from.Address))
				{
					Log?.Invoke($"PUNCHWAKE from {from} - rate limited");
					return;
				}

				HandlePunchWake(parts[1], parts[2], from);
				break;

			// Fire-and-forget: sent the instant a host turns hosting off.
			case "UNREGISTER" when parts.Length == 3:
				if (!_refreshLimiter.Allow(from.Address))
					return;

				HandleUnregister(parts[1], parts[2], from);
				break;

			default:
				Send(from, "ERR BADCMD");
				break;
		}
	}

	void HandleRegister(IPEndPoint from)
	{
		if (_sessions.Count >= MaxConcurrentSessions)
		{
			Log?.Invoke($"REGISTER from {from} - refused, at capacity ({MaxConcurrentSessions})");
			Send(from, "ERR BUSY");
			return;
		}

		// Collision-safe: TryAdd fails harmlessly on a collision and the loop tries a fresh code.
		RendezvousSession session;
		while (true)
		{
			session = new RendezvousSession
			{
				Id = RandomCode(),
				Password = RandomCode(),
				HostEndpoint = from,
				LastSeenUtc = DateTime.UtcNow,
			};

			if (_ownership?.IsProtected(session.Id) != true && _sessions.TryAdd(session.Id, session))
				break;
		}

		Log?.Invoke($"REGISTER from {from} -> issued {session.Id} ({_sessions.Count} active)");
		Send(from, $"REGISTERED {session.Id} {session.Password}");
	}

	// "REGISTER <id> <password>": (1) id live with matching password -> keep alive, same pair;
	// (2) id free -> grant exactly that pair; (3) taken/malformed/at capacity -> mint fresh.
	void HandleRegisterReclaim(string id, string password, IPEndPoint from)
	{
		if (_sessions.Count >= MaxConcurrentSessions)
		{
			Log?.Invoke($"REGISTER (reclaim) from {from} - refused, at capacity ({MaxConcurrentSessions})");
			Send(from, "ERR BUSY");
			return;
		}

		if (!IsValidCredential(id) || !IsValidCredential(password))
		{
			Log?.Invoke($"REGISTER (reclaim) from {from} - malformed id/password, minting fresh instead");
			HandleRegister(from);
			return;
		}

		if (_sessions.TryGetValue(id, out var existing))
		{
			if (CredentialsMatch(existing, password))
			{
				existing.HostEndpoint = from;
				existing.LastSeenUtc = DateTime.UtcNow;
				Log?.Invoke($"REGISTER (reclaim) from {from} -> resumed {id}");
				Send(from, $"REGISTERED {existing.Id} {existing.Password}");
				return;
			}

			Log?.Invoke($"REGISTER (reclaim) from {from} - {id} is taken, minting fresh instead");
			HandleRegister(from);
			return;
		}

		var session = new RendezvousSession
		{
			Id = id,
			Password = password,
			HostEndpoint = from,
			LastSeenUtc = DateTime.UtcNow,
		};

		if (!_sessions.TryAdd(id, session))
		{
			Log?.Invoke($"REGISTER (reclaim) from {from} - lost a race for {id}, minting fresh instead");
			HandleRegister(from);
			return;
		}

		Log?.Invoke($"REGISTER (reclaim) from {from} -> restored {id} ({_sessions.Count} active)");
		Send(from, $"REGISTERED {session.Id} {session.Password}");
	}

	static bool IsValidCredential(string s) =>
		s.Length == CredentialLength && s.All(CredentialAlphabet.Contains);

	void HandleConnect(string id, string password, IPEndPoint from)
	{
		if (!_sessions.TryGetValue(id, out var session) || !CredentialsMatch(session, password))
		{
			Log?.Invoke($"CONNECT from {from} - no match for {id}");
			Send(from, "ERR NOTFOUND");
			return;
		}
		session.LastSeenUtc = DateTime.UtcNow;

		Log?.Invoke($"CONNECT from {from} - paired with host {session.HostEndpoint}");
		Send(from, $"PEER {session.HostEndpoint.Address} {session.HostEndpoint.Port}");
		Send(session.HostEndpoint, $"PEER {from.Address} {from.Port}");
	}

	void HandleRefresh(string id, string password, IPEndPoint from)
	{
		if (!_sessions.TryGetValue(id, out var session) || !CredentialsMatch(session, password))
		{
			Log?.Invoke($"REFRESH from {from} - no match for {id}");
			Send(from, "ERR NOTFOUND");
			return;
		}

		session.HostEndpoint = from;
		session.LastSeenUtc = DateTime.UtcNow;

		bool wake = session.WakePending;
		session.WakePending = false;
		Log?.Invoke(wake
			? $"REFRESH from {from} - {id} kept alive, delivering WAKE"
			: $"REFRESH from {from} - {id} kept alive");
		Send(from, wake ? "WAKE" : "OK");
	}

	void HandlePunchWake(string id, string password, IPEndPoint from)
	{
		if (!_sessions.TryGetValue(id, out var session) || !CredentialsMatch(session, password))
		{
			Log?.Invoke($"PUNCHWAKE from {from} - no match for {id}");
			Send(from, "ERR NOTFOUND");
			return;
		}

		session.WakePending = true;
		Log?.Invoke($"PUNCHWAKE from {from} - {id} flagged, fires on the host's next REFRESH");
		Send(from, "OK");
	}

	// Immediate "a host stopped hosting" signal (the sweep is only the fallback for power loss).
	void HandleUnregister(string id, string password, IPEndPoint from)
	{
		if (!_sessions.TryGetValue(id, out var session) || !CredentialsMatch(session, password))
			return;

		if (_sessions.TryRemove(id, out _))
			session.HostTcp?.Dispose();
		Log?.Invoke($"Session {id} unregistered - host {from} turned hosting off ({_sessions.Count} still active)");
	}

	// ---------------------------------------------------------------- TCP punch signaling

	async Task TcpAcceptLoopAsync(TcpListener listener, CancellationToken cancel)
	{
		while (!cancel.IsCancellationRequested)
		{
			TcpClient client;
			try
			{
				client = await listener.AcceptTcpClientAsync(cancel).ConfigureAwait(false);
			}
			catch (Exception) when (cancel.IsCancellationRequested)
			{
				break;
			}
			catch (Exception ex)
			{
				Log?.Invoke($"TCP accept failed: {ex.Message}");
				continue;
			}

			_ = HandleTcpConnection(client);
		}
	}

	async Task HandleTcpConnection(TcpClient client)
	{
		var observed = (IPEndPoint?)client.Client.RemoteEndPoint;
		string remote = observed?.ToString() ?? "?";

		try
		{
			var stream = client.GetStream();
			client.NoDelay = true;

			using var readCts = new CancellationTokenSource(TimeSpan.FromSeconds(10));
			string? line = await ReadLine(stream, readCts.Token).ConfigureAwait(false);
			if (line is null || observed is null)
			{
				client.Dispose();
				return;
			}

			var parts = line.Split(' ', StringSplitOptions.RemoveEmptyEntries);
            if(line==OwnerEnrollmentProtocol.Command) {
                if(!EnrollLimiter.Allow(observed.Address)) {
                    Log?.Invoke($"ENROLL from {remote} - rate limited");
                    await OwnerEnrollmentProtocol.WriteFrame(stream,new byte[]{OwnerEnrollmentProtocol.Busy},readCts.Token);
                    client.Dispose();return;
                }
                await OwnerEnrollmentService.Handle(stream,_ownership,PairingPasswordFor,Log,remote,readCts.Token);
                client.Dispose();return;
            }
            if(line=="BRIDGEOWNER1") {
                if(!_ownerLimiter.Allow(observed.Address)) {client.Dispose();return;}
                await NativeOwnerService.Handle(stream,_ownership,((IPEndPoint)_tcpListener!.LocalEndpoint).Port,readCts.Token);
                client.Dispose();return;
            }
			if (parts.Length != 3 || !_connectLimiter.Allow(observed.Address))
			{
				await WriteLine(stream, "ERR BADCMD").ConfigureAwait(false);
				client.Dispose();
				return;
			}

			if (parts[0] == "PUNCHHOST")
				await HandlePunchHost(client, stream, parts[1], parts[2], observed, remote).ConfigureAwait(false);
			else if (parts[0] == "PUNCHHOST2")
				await HandlePunchHost(client, stream, parts[1], parts[2], observed, remote, ticketCapable:true).ConfigureAwait(false);
			else if (parts[0] == "PUNCHHOSTAUTH")
				await HandlePunchHost(client, stream, parts[1], parts[2], observed, remote, authenticated:true).ConfigureAwait(false);
			else if (parts[0] == "PUNCHHOSTAUTH2") // provisioned (direct-owner) hosts that understand the connection ticket
				await HandlePunchHost(client, stream, parts[1], parts[2], observed, remote, authenticated:true, ticketCapable:true).ConfigureAwait(false);
			else if (parts[0] == "BRIDGEHOST")
				await HandleBridgeHost(client,stream,parts[1],parts[2],observed).ConfigureAwait(false);
			else if (parts[0] == "PUNCHJOIN")
				await HandlePunchJoin(client, stream, parts[1], parts[2], observed, remote).ConfigureAwait(false);
			else if (parts[0] == "PUNCHJOIN2")
				await HandlePunchJoin(client, stream, parts[1], parts[2], observed, remote, wantTicket:true).ConfigureAwait(false);
			else
			{
				await WriteLine(stream, "ERR BADCMD").ConfigureAwait(false);
				client.Dispose();
			}
		}
		catch (Exception ex)
		{
			Log?.Invoke($"[{remote}] TCP connection error: {ex.Message}");
			client.Dispose();
		}
	}

	// The connection STAYS OPEN: it's the signaling channel PEER arrives on later. The host is
	// expected to close it itself right after receiving PEER (to free the local port for the
	// punch attempt).
	async Task HandlePunchHost(TcpClient host, NetworkStream stream, string id, string password, IPEndPoint observed, string remote, bool authenticated=false, bool ticketCapable=false)
	{
		RendezvousSession? session = null;
		var directRoute = authenticated ? _direct?.ConsumeTicket(id,password,observed.Address) : null;
		string? Attach()
		{
			if (!_sessions.TryGetValue(id, out session) || !CredentialsMatch(session, password))
				return "ERR NOTFOUND";
			session.LastSeenUtc = DateTime.UtcNow;
			var previous = session.HostTcp;
			session.HostTcp = host;
			session.HostTcpStream = stream;
			session.HostTcpObserved = observed;
			session.HostTicketCapable = ticketCapable;
			previous?.Dispose();
			return null;
		}
		string? error;
		if (authenticated) {
			error = directRoute is null ? "ERR OWNERAUTH" : _ownership!.Current<string?>(id,directRoute.Generation,HostKind.Direct,route=> {
				if (!_sessions.TryGetValue(id,out session) || session.OwnerGeneration != route.Generation) {
					session?.HostTcp?.Dispose();
					session = new RendezvousSession {Id=id,Password="",HostEndpoint=observed,LastSeenUtc=DateTime.UtcNow,OwnerGeneration=route.Generation};
					_sessions[id] = session;
				}
				session.HostTcp?.Dispose(); session.HostTcp=host; session.HostTcpStream=stream; session.HostTcpObserved=observed;
				session.HostTicketCapable=ticketCapable;
				return null;
			},()=>"ERR OWNERAUTH");
		} else error = _ownership is null ? Attach() : _ownership.Legacy(id, Attach, () => "ERR OWNERAUTH");
		if (error is not null)
		{
			Log?.Invoke($"[{remote}] PUNCHHOST {id} - no live session with those credentials, rejecting");
			await WriteLine(stream, error).ConfigureAwait(false);
			host.Dispose();
			return;
		}
		var attached = session!;

		Log?.Invoke($"[{remote}] PUNCHHOST {id} registered, observed address {observed}");
		await WriteLine(stream, "OK").ConfigureAwait(false);

		// Read and discard until the host closes it - lets us notice the host going away.
		try
		{
			byte[] buf = new byte[32];
			while (await stream.ReadAsync(buf).ConfigureAwait(false) > 0) { }
		}
		catch (Exception ex)
		{
			Log?.Invoke($"[{id}] host TCP connection ended: {ex.Message}");
		}
		finally
		{
			if (ReferenceEquals(attached.HostTcp, host))
			{
				attached.HostTcp = null;
				attached.HostTcpStream = null;
				attached.HostTcpObserved = null;
			}
			host.Dispose();
			Log?.Invoke($"[{id}] host TCP signaling closed");
		}
	}

	async Task HandleBridgeHost(TcpClient host,NetworkStream stream,string id,string token,IPEndPoint observed)
	{
		var ticket=_ownership?.PunchTickets.Consume(id,token);
		RendezvousSession? attached=null;
		bool admitted=ticket is not null&&_ownership!.Current(id,ticket.Generation,HostKind.UsbBridge,route=> {
			if(route.Owner!=ticket.Owner) return false;
			if(!_sessions.TryGetValue(id,out attached)||attached.OwnerGeneration!=route.Generation) {
				attached?.HostTcp?.Dispose();
				attached=new RendezvousSession {Id=id,Password="",HostEndpoint=observed,LastSeenUtc=DateTime.UtcNow,OwnerGeneration=route.Generation};
				_sessions[id]=attached;
			}
			attached.HostTcp?.Dispose();attached.HostTcp=host;attached.HostTcpStream=stream;attached.HostTcpObserved=observed;
			return true;
		},()=>false);
		if(!admitted) {await WriteLine(stream,"ERR OWNERAUTH");host.Dispose();return;}
		try {
			await WriteLine(stream,$"OK {observed}");
			// Bounded wait also closes registrations whose ownership expired or changed.
			var buffer=new byte[1];
			var read=stream.ReadAsync(buffer).AsTask();
			while(!read.IsCompleted) {
				await Task.WhenAny(read,Task.Delay(1000));
				if(!_ownership!.Current(id,ticket!.Generation,HostKind.UsbBridge,route=>route.Owner==ticket.Owner,()=>false)) break;
			}
			host.Dispose();
			try {await read;} catch(IOException) { } catch(ObjectDisposedException) { }
		} finally {
			if(ReferenceEquals(attached!.HostTcp,host)) {attached.HostTcp=null;attached.HostTcpStream=null;attached.HostTcpObserved=null;}
			host.Dispose();
		}
	}

	// wantTicket (PUNCHJOIN2): the connector understands the connection ticket. When the host does too
	// (PUNCHHOST2) the relay mints a random one-time 48-bit ticket and appends it to BOTH existing lines
	// ("OK <host> <ticket>" / "PEER <connector> <ticket>"). The host then accepts a mapped-port connection
	// from ANY source address that presents the ticket in its handshake request, so NAT rewriting of the
	// source (a join from inside the host's own house) no longer matters. Old peers use the old verbs and
	// never see a ticket; the IP pin stays the rule for them.
	async Task HandlePunchJoin(TcpClient connector, NetworkStream stream, string id, string password, IPEndPoint observed, string remote, bool wantTicket=false)
	{
        var bridgeRoute=_ownership?.Resolve(id);
        if(bridgeRoute?.Kind==HostKind.UsbBridge) {
            if(!AcceptsBridge(bridgeRoute,id,password)) {await WriteLine(stream,"ERR HOSTUNREACHABLE");connector.Dispose();return;}
            (NetworkStream Stream,IPEndPoint Endpoint)? ready=
                _ownership!.Current<(NetworkStream,IPEndPoint)?>(id,bridgeRoute.Generation,HostKind.UsbBridge,route=> {
                    if(!AcceptsBridge(route,id,password)||!_sessions.TryGetValue(id,out var host)||host.OwnerGeneration!=route.Generation||
                        host.HostTcpStream is null||host.HostTcpObserved is null) return null;
                    var matched=(host.HostTcpStream,host.HostTcpObserved);
                    host.HostTcpStream=null;host.HostTcpObserved=null;
                    return matched;
                },()=>null);
            if(ready is null) {await WriteLine(stream,"ERR NOTFOUND");connector.Dispose();return;}
            try {
                await WriteLine(ready.Value.Stream,$"PEER {observed}");
                await WriteLine(stream,$"OK {ready.Value.Endpoint}");
            } catch(IOException) { }
            finally {connector.Dispose();}
            return;
        }
		bool protectedId = _ownership?.IsFenced(id) == true;
		var directRoute = protectedId ? _ownership!.Resolve(id) : null;
		if (protectedId && (directRoute is null || !DirectOwnerWire.Accepts(directRoute,password)))
		{
			await WriteLine(stream, "ERR HOSTUNREACHABLE").ConfigureAwait(false);
			connector.Dispose();
			return;
		}
		if (!_sessions.TryGetValue(id, out var session) ||
			(protectedId ? session.OwnerGeneration != directRoute!.Generation : !CredentialsMatch(session, password)) ||
			session.HostTcpStream is null || session.HostTcpObserved is null)
		{
			Log?.Invoke($"[{remote}] PUNCHJOIN {id} - no matching/ready host, rejecting");
			await WriteLine(stream, "ERR NOTFOUND").ConfigureAwait(false);
			connector.Dispose();
			return;
		}

		var hostStream = session.HostTcpStream;
		var hostObserved = session.HostTcpObserved;
		session.LastSeenUtc = DateTime.UtcNow;
		string? ticket = wantTicket && session.HostTicketCapable
			? Convert.ToHexString(RandomNumberGenerator.GetBytes(6)).ToLowerInvariant()
			: null;
		Log?.Invoke($"[{remote}] PUNCHJOIN {id} - connector observed at {observed}, host observed at {hostObserved}");

		try
		{
			await WriteLine(hostStream, ticket is null ? $"PEER {observed}" : $"PEER {observed} {ticket}").ConfigureAwait(false);
		}
		catch (Exception ex)
		{
			Log?.Invoke($"[{id}] failed to notify host of peer: {ex.Message}");
			await WriteLine(stream, "ERR HOSTUNREACHABLE").ConfigureAwait(false);
			connector.Dispose();
			return;
		}

		await WriteLine(stream, ticket is null ? $"OK {hostObserved}" : $"OK {hostObserved} {ticket}").ConfigureAwait(false);
		connector.Dispose();
		Log?.Invoke($"[{id}] address exchange complete{(ticket is null ? "" : " (connection ticket issued)")}, rendezvous stepping out of the way");
	}

    static bool AcceptsBridge(HostRoute route,string id,string password)
    {
        try {return NXRemotePlay.Protocol.NativeBridgeRoute.Parse(route.Endpoint).Accepts(id,password);}
        catch(ArgumentException) {return false;}
        catch(FormatException) {return false;}
    }

	static async Task<string?> ReadLine(NetworkStream stream, CancellationToken cancel)
	{
		var sb = new StringBuilder();
		byte[] one = new byte[1];

		while (sb.Length < MaxTcpLineLength)
		{
			int r = await stream.ReadAsync(one, cancel).ConfigureAwait(false);
			if (r <= 0)
				return null;

			if (one[0] == (byte)'\n')
				return sb.ToString().TrimEnd('\r');

			sb.Append((char)one[0]);
		}

		return null;
	}

	static Task WriteLine(NetworkStream stream, string line)
	{
		byte[] bytes = Encoding.ASCII.GetBytes(line + "\n");
		return stream.WriteAsync(bytes).AsTask();
	}

	// ---------------------------------------------------------------- helpers

	// The live pairing password for an id, or null (unknown id, expired, or a host that attached through an
	// already-protected route and so holds no pairing password).
	string? PairingPasswordFor(string id)
	{
		if (!_sessions.TryGetValue(id, out var session)) return null;
		return string.IsNullOrEmpty(session.Password) ? null : session.Password;
	}

	static bool CredentialsMatch(RendezvousSession session, string password)
	{
		byte[] expected = Encoding.UTF8.GetBytes(session.Password);
		byte[] supplied = Encoding.UTF8.GetBytes(password);
		return expected.Length == supplied.Length &&
			CryptographicOperations.FixedTimeEquals(expected, supplied);
	}

	void Send(IPEndPoint to, string message)
	{
		byte[] bytes = Encoding.UTF8.GetBytes(message);
		_socket!.Send(bytes, bytes.Length, to);
	}

	static string RandomCode()
	{
		var chars = new char[CredentialLength];
		for (int i = 0; i < chars.Length; i++)
			chars[i] = CredentialAlphabet[RandomNumberGenerator.GetInt32(CredentialAlphabet.Length)];
		return new string(chars);
	}
}
