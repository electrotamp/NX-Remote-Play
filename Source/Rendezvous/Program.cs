namespace Rendezvous;

// Session ID/Password signaling with TCP hole-punch coordination and wake.
// See RendezvousServer.cs for the protocol.
class Program
{
	// Default shared UDP/TCP signaling port.
	const int DefaultPort = 7921;

	static async Task<int> Main(string[] args)
	{
        if(args.Length==3&&args[0]=="--enroll"){
            try{TrustedEnrollment.Import(args[1],args[2]);Console.WriteLine("Trusted console identity enrolled. Start rendezvous with --ownership-state pointing to this registry.");return 0;}
            catch(Exception error){Console.Error.WriteLine("Enrollment failed: "+error.Message);return 1;}
        }
		int port = DefaultPort;
		string? ownershipPath = null;
		if (args.Length > 0)
		{
			if (args[0] is "--help" or "-h")
			{
				PrintHelp();
				return 0;
			}

			if (!int.TryParse(args[0], out port))
			{
				Console.WriteLine($"Invalid port '{args[0]}'.");
				return 1;
			}
			for (int index = 1; index < args.Length;)
			{
				if (args[index] == "--ownership-state" && index + 1 < args.Length && ownershipPath is null && File.Exists(args[index + 1])) {
					ownershipPath = args[index + 1]; index += 2;
				} else { Console.WriteLine("Invalid service options or missing provisioned registry file."); return 1; }
			}
		}

		if (port is < 1 or > 65535) return 1;
        if (ownershipPath is null)
        {
            try { ownershipPath = DefaultServerSetup.Prepare(DefaultServerSetup.DirectoryPath); }
            catch (Exception error) { Console.Error.WriteLine("Server setup failed: " + error.Message); return 1; }
        }
		using var ownership = ownershipPath is null ? null : new HostOwnership(ownershipPath);
        Console.WriteLine("NX-Remote-Play - UDP media rendezvous enabled; endpoint discovery only (registration diagnostics)");
        Console.WriteLine("Automatic USB hosting support enabled. Native clients use the existing ID/password and port.");
		var server = new RendezvousServer(ownership);
		server.Log += line => Console.WriteLine($"{DateTime.Now:HH:mm:ss} {line}");

		using var cts = new CancellationTokenSource();
		Console.CancelKeyPress += (_, e) =>
		{
			e.Cancel = true;
			cts.Cancel();
		};

		string? localIp = AddressDetection.DetectLocalIp();
		string? publicIp = await AddressDetection.DetectPublicIp();

		Console.WriteLine();
		Console.WriteLine("Address(es) for clients:");
		Console.WriteLine(localIp is not null
			? $"  Same network (LAN):  {localIp}:{port}"
			: "  Same network (LAN):  could not detect");
		Console.WriteLine(publicIp is not null
			? $"  Internet (WAN):      {publicIp}:{port}  (needs TCP AND UDP port {port} forwarded to this machine)"
			: "  Internet (WAN):      could not detect");
		Console.WriteLine();
		Console.WriteLine("Press Ctrl+C to stop.");

		await server.RunAsync(port, cts.Token);
		return 0;
	}

	static void PrintHelp()
	{
		Console.WriteLine("NX-Remote-Play Rendezvous - Session ID/Password signaling + TCP punch coordination");
		Console.WriteLine();
		Console.WriteLine($"Usage: Rendezvous [port] [--ownership-state <existing registry>] (default {DefaultPort}, UDP/TCP)");
        Console.WriteLine("Native Bridge Mode ownership works on the same TCP port when --ownership-state is configured.");
        Console.WriteLine("Normal launch automatically loads the protected per-user registry and imports an existing trusted local console record. No special launcher is needed.");
        Console.WriteLine("Local enrollment, with the service stopped: Rendezvous --enroll <registry.json> <private 48-byte console record>");
		Console.WriteLine("Enrollment is trusted/local only. Keep enrollment records and the registry private.");
		Console.WriteLine("UDP:  REGISTER [<id> <pw>] -> REGISTERED <id> <pw> | REFRESH <id> <pw> -> OK|WAKE");
		Console.WriteLine("      PUNCHWAKE <id> <pw> -> OK | UNREGISTER <id> <pw> | CONNECT <id> <pw> -> PEER");
		Console.WriteLine("TCP:  PUNCHHOST <id> <pw> -> OK ... PEER <ip:port> | PUNCHJOIN <id> <pw> -> OK <host ip:port>");
	}
}
