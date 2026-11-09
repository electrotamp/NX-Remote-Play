using LibUsbDotNet.LibUsb;
using NxRemotePlay.Client.Core;
using System;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using System.Buffers.Binary;
using System.Text;
using System.IO;
using System.Diagnostics;

namespace NxRemotePlay.Client.Sources
{
    class UsbStreamingSource : StreamingSource, IInputSink, NxRemotePlay.Client.Bridge.ISlotInputSink, NxRemotePlay.Client.Bridge.IBridgeHostSource, NxRemotePlay.Client.Bridge.IOwnerEnrollmentTarget
    {
        const uint CapabilityMarker = 0x3155534E;
        readonly SemaphoreSlim inputWrites = new(1);
        uint inputSequence;
        ulong inputSession;
        readonly bool negotiateControls;
        readonly bool negotiateOwner;
        // Desktop mode: any USB failure ends this source instead of reconnecting. Both consoles report the same
        // USB serial, so a reconnect cannot tell them apart and could attach this session to a different console.
        readonly bool failFast;
        static readonly TimeSpan UsbIdleLimit = TimeSpan.FromSeconds(10);
        readonly SemaphoreSlim ownerRequests = new(1);
        readonly object ownerGate = new();
        TaskCompletionSource<byte[]>? pendingOwner;
        uint ownerToken;
        uint pendingReplyMagic;
        public bool SupportsOwnerProof { get; private set; }
        public bool SupportsDelegation { get; private set; }
        // True when this console has no owner key yet and can have one installed (see OwnerKeyEnrollment).
        public bool CanEnroll { get; private set; }
        bool enrollmentNegotiated;
        public bool SupportsInput { get; private set; }
        public string SessionId { get; private set; } = "";
        public string SessionPassword { get; private set; } = "";
        // Capability bit 64: the console accepts per-viewer pads in slots 1..n (multi-viewer firmware); byte 13 is n.
        public int ExtraSlotCapacity { get; private set; }
        bool closed = false;
        readonly DvrUsbDevice device;
        protected UsbEndpointReader reader;
        protected UsbEndpointWriter writer;
        readonly UsbPacketReader packets;
        readonly object transferTraceGate = new();
        readonly StreamWriter? transferTrace;
        readonly Stopwatch transferWindow = Stopwatch.StartNew();
        long lastTransferEnd;
        int transferCount;
        long transferBytes;
        double maxReadMs, maxIdleBeforeReadMs, maxCompletionGapMs;

        static StreamWriter? OpenTransferTrace()
        {
            try
            {
                string folder = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "NX-Remote-Play", "Diagnostics");
                Directory.CreateDirectory(folder);
                var writer = new StreamWriter(Path.Combine(folder, $"usb-flow-{DateTime.UtcNow:yyyyMMdd-HHmmss}-{Guid.NewGuid():N}.csv"));
                writer.WriteLine("utc,window_ms,transfers,bytes,max_read_ms,max_idle_before_read_ms,max_completion_gap_ms");
                return writer;
            }
            catch (Exception) { return null; }
        }

        void TraceTransfer(long start, long end, int bytes)
        {
            if (transferTrace is null) return;
            lock (transferTraceGate)
            {
                transferCount++;
                transferBytes += bytes;
                maxReadMs = Math.Max(maxReadMs, Stopwatch.GetElapsedTime(start, end).TotalMilliseconds);
                if (lastTransferEnd != 0)
                {
                    maxIdleBeforeReadMs = Math.Max(maxIdleBeforeReadMs, Stopwatch.GetElapsedTime(lastTransferEnd, start).TotalMilliseconds);
                    maxCompletionGapMs = Math.Max(maxCompletionGapMs, Stopwatch.GetElapsedTime(lastTransferEnd, end).TotalMilliseconds);
                }
                lastTransferEnd = end;
                if (transferWindow.ElapsedMilliseconds < 1000) return;
                try
                {
                    transferTrace.WriteLine($"{DateTime.UtcNow:O},{transferWindow.ElapsedMilliseconds},{transferCount},{transferBytes},{maxReadMs:F2},{maxIdleBeforeReadMs:F2},{maxCompletionGapMs:F2}");
                    transferTrace.Flush();
                }
                catch (Exception) { /* Diagnostics must never interrupt USB media. */ }
                transferWindow.Restart();
                transferCount = 0; transferBytes = 0;
                maxReadMs = maxIdleBeforeReadMs = maxCompletionGapMs = 0;
            }
        }

        public UsbStreamingSource(DvrUsbDevice device, StreamingOptions opt, CancellationToken cancel, bool negotiateControls = true, bool negotiateOwner = false, bool failFast = false) : base(opt, cancel)
        {
            this.failFast = failFast;
            this.negotiateControls = negotiateControls;
            this.negotiateOwner = negotiateOwner;
            this.device = device;
            (reader, writer) = device.Open();
            transferTrace = OpenTransferTrace();
            packets = new UsbPacketReader(buffer => UsbPacketReader.ReadPatiently(async () =>
            {
                long readStart = Stopwatch.GetTimestamp();
                var result = await reader.ReadAsync(buffer, 3000).ConfigureAwait(false);
                TraceTransfer(readStart, Stopwatch.GetTimestamp(), result.transferLength);
                return result;
            }, UsbIdleLimit, Cancellation));
        }

        public override Task StopStreaming()
        {
            Dispose();
            return Task.CompletedTask;
        }

        bool Reconnect(string reason)
        {
            ReportMessage(string.Format(Program.Strings.Errors.UsbResetMessage, reason));
            if (device.TryReconnect())
            {
                (reader, writer) = device.Open();
                return true;
            }
            return false;
        }

        public override async Task Connect()
        {
            await WaitForConnection().ConfigureAwait(false);
        }

        async Task WaitForConnection()
        {
            while (!Cancellation.IsCancellationRequested)
            {
                Program.DebugLog($"Sending USB connection request");

                try
                {
                    await DoHandshake(Options.Kind).ConfigureAwait(false);
                }
                catch (Exception e)
                {
                    // Drain the bounded handshake before closing/reopening the handle.
                    // Cancellation must not initiate another native reconnect.
                    Cancellation.ThrowIfCancellationRequested();
                    if (failFast) throw new IOException("The Switch did not accept the USB connection (it may still be closing the previous session). Reconnecting automatically.", e);
                    ReportMessage(Program.Strings.Errors.UsbTimeoutError);

                    if (!Cancellation.IsCancellationRequested)
                        await Task.Delay(1000, Cancellation).ConfigureAwait(false);

                    Reconnect(e.Message);
                    continue;
                }

                Cancellation.ThrowIfCancellationRequested();
                return;
            }
            Cancellation.ThrowIfCancellationRequested();
        }

        public override async Task Flush()
        {
            if (Cancellation.IsCancellationRequested)
                return;

            if (failFast) throw new IOException("The Switch's USB connection ended.");

            // Wait some time so the switch side timeouts
            await Task.Delay(3000, Cancellation).ConfigureAwait(false);

            // Then attempt to connect again
            await WaitForConnection().ConfigureAwait(false);
        }

        public override async Task<ReceivedPacket> ReadNextPacket()
        {
            while (true)
            {
                var packet = await packets.Next().ConfigureAwait(false);
                var header = packet.Header;
                if (header.Magic == UsbEnrollmentProtocol.ResponseMagic)
                {
                    try
                    {
                        if (!enrollmentNegotiated) throw new Exception("Unnegotiated USB enrollment response.");
                        var bytes = new byte[82]; MemoryMarshal.Write(bytes, in header); packet.Buffer.Span.CopyTo(bytes.AsSpan(18));
                        var response = UsbEnrollmentProtocol.Response(bytes);
                        // A late or duplicate reply (no matching request in flight) is dropped, never fatal.
                        lock (ownerGate)
                            if (response.Token == ownerToken && pendingOwner is not null && pendingReplyMagic == header.Magic)
                            {
                                if (response.Status == 0) pendingOwner.TrySetResult(response.Fingerprint);
                                else pendingOwner.TrySetException(new Exception(UsbEnrollmentProtocol.Describe(response.Status)));
                            }
                    }
                    finally { packet.Buffer?.Free(); }
                    continue;
                }
                if (header.Magic == UsbOwnerProtocol.ResponseMagic || header.Magic == UsbDelegationProtocol.ResponseMagic)
                {
                    try
                    {
                        if (!SupportsOwnerProof || header.Magic == UsbDelegationProtocol.ResponseMagic && !SupportsDelegation) throw new Exception("Unnegotiated USB owner response.");
                        var bytes = new byte[82]; MemoryMarshal.Write(bytes, in header); packet.Buffer.Span.CopyTo(bytes.AsSpan(18));
                        var response = UsbOwnerProtocol.Response(bytes, header.Magic);
                        lock (ownerGate)
                            if (response.Token == ownerToken && pendingOwner is not null && pendingReplyMagic == header.Magic)
                            {
                                if (response.Status == 0) pendingOwner.TrySetResult(response.Proof);
                                else pendingOwner.TrySetException(new Exception("Switch rejected USB owner authorization (status " + response.Status + ")."));
                            }
                    }
                    finally { packet.Buffer?.Free(); }
                    continue;
                }

                if (!ValidatePacketHeader(in header))
                {
                    packet.Buffer?.Free(); throw new Exception($"Invalid packet header: {header}");
                }
                return packet;
            }
        }

        protected override async Task<byte[]> ReadHandshakeHello(StreamKind stream, int maxBytes)
        {
            var buffer = new byte[maxBytes];

            var (err, read) = await reader.ReadAsync(buffer, 800).ConfigureAwait(false);
            if (err != LibUsbDotNet.Error.Success || read != maxBytes)
                throw new Exception($"libusb receive handshake failed, result: {err} len: {read}");

            return buffer;
        }

        protected override async Task<byte[]> SendHandshakePacket(ProtoHandshakeRequest req, int expectedSize)
        {
            SupportsInput = false;
            packets.Reset();
            ResetOwnerProof();
            SessionId = SessionPassword = "";
            ExtraSlotCapacity = 0; inputSession = 0;
            req.RequestUsbControls = negotiateControls && req.Version == ProtocolUtil.ProtocolVersion3 && !req.PerformMemoryDiagnostic;
            req.RequestUsbOwner = negotiateOwner && req.RequestUsbControls;
            var buffer = new byte[ProtoHandshakeRequest.StructureSize];
            MemoryMarshal.Write(buffer, in req);

            var (err, transfer) = await writer.WriteAsync(buffer, 1500).ConfigureAwait(false);

            if (err != LibUsbDotNet.Error.Success || transfer != buffer.Length)
                throw new Exception($"libusb write handshake failed, result: {err} len: {transfer}");

            var response = new byte[expectedSize];
            (err, transfer) = await reader.ReadAsync(response, 0, expectedSize, 1500).ConfigureAwait(false);
            if (err != LibUsbDotNet.Error.Success || transfer != expectedSize)
                throw new Exception($"libusb receive handshake failed, result: {err} len: {transfer}/{expectedSize}");

            if (req.RequestUsbControls && expectedSize == 72 && BinaryPrimitives.ReadUInt32LittleEndian(response.AsSpan(4)) == CapabilityMarker)
            {
                var capabilities = new byte[256];
                (err, transfer) = await reader.ReadAsync(capabilities, 0, capabilities.Length, 1500).ConfigureAwait(false);
                if (err != LibUsbDotNet.Error.Success || transfer != capabilities.Length)
                    throw new Exception("USB capability response was incomplete.");
                ParseCapabilities(capabilities);
            }

            return response;
        }

        internal void ParseCapabilities(byte[] bytes)
        {
            ValidateCapabilities(bytes);
            uint caps = BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(8));
            SupportsInput = (caps & 6) == 6;
            SupportsOwnerProof = negotiateOwner && (caps & 8) != 0;
            SupportsDelegation = SupportsOwnerProof && (caps & 16) != 0;
            enrollmentNegotiated = EnrollmentOffered(caps, negotiateOwner);
            CanEnroll = enrollmentNegotiated;
            ExtraSlotCapacity = (caps & 64) != 0 ? Math.Min((int)bytes[13], 3) : 0;
            Bridge.PadFlowLog.Event("capabilities", 0, $"extra_slots={ExtraSlotCapacity} advertised={bytes[13]} slots_byte={bytes[12]} input={SupportsInput}");
            inputSession = BinaryPrimitives.ReadUInt64LittleEndian(bytes.AsSpan(208));
            string Field(int offset, int length) => Encoding.UTF8.GetString(bytes, offset, length).TrimEnd('\0');
            SessionId = Field(16, 32); SessionPassword = Field(48, 64);
        }

        // The console offers first-use key setup only when owner mode was requested, it has no usable key, and its firmware can install one.
        internal static bool EnrollmentOffered(uint capabilities, bool negotiateOwner) =>
            negotiateOwner && (capabilities & 8) == 0 && (capabilities & UsbEnrollmentProtocol.CapabilityBit) != 0;

        internal static void ValidateCapabilities(byte[] bytes)
        {
            if (bytes.Length != 256 || BinaryPrimitives.ReadUInt32LittleEndian(bytes) != CapabilityMarker ||
                BinaryPrimitives.ReadUInt16LittleEndian(bytes.AsSpan(4)) != 1 || BinaryPrimitives.ReadUInt16LittleEndian(bytes.AsSpan(6)) != 256 || bytes[12] != 1 ||
                BinaryPrimitives.ReadUInt64LittleEndian(bytes.AsSpan(208)) == 0)
                throw new Exception("Unsupported USB capabilities.");
        }

        internal static byte[] ControlFrame(byte[] packet, uint sequence, ulong session = 1, byte slot = 0)
        {
            byte operation = packet.Length == 17 && packet[0] == 2 ? (byte)1 :
                packet.Length == 5 && packet[0] <= 1 ? (byte)2 :
                packet.Length == 5 && packet[0] == 3 ? (byte)3 : throw new ArgumentException("Invalid USB input packet.");
            var frame = new byte[64];
            BinaryPrimitives.WriteUInt32LittleEndian(frame, 0x3143584E);
            BinaryPrimitives.WriteUInt16LittleEndian(frame.AsSpan(4), 1);
            BinaryPrimitives.WriteUInt16LittleEndian(frame.AsSpan(6), (ushort)packet.Length);
            BinaryPrimitives.WriteUInt32LittleEndian(frame.AsSpan(8), sequence);
            frame[12] = operation;
            frame[14] = slot;
            BinaryPrimitives.WriteUInt64LittleEndian(frame.AsSpan(16), session);
            packet.CopyTo(frame, 24);
            return frame;
        }

        public Task SendInput(byte[] packet) => SendInput(0, packet);

        // Slot 0 is the hosting desktop's pad; slots 1..ExtraSlotCapacity are remote viewers' pads.
        public async Task SendInput(int slot, byte[] packet)
        {
            if (!SupportsInput || closed || Cancellation.IsCancellationRequested) return;
            if (slot < 0 || slot > ExtraSlotCapacity) throw new ArgumentOutOfRangeException(nameof(slot));
            await inputWrites.WaitAsync(Cancellation).ConfigureAwait(false);
            try
            {
                if (closed || Cancellation.IsCancellationRequested) return;
                var frame = ControlFrame(packet, ++inputSequence, inputSession, (byte)slot);
                var (err, transfer) = await writer.WriteAsync(frame, 500).ConfigureAwait(false);
                if (err != LibUsbDotNet.Error.Success || transfer != frame.Length)
                    throw new Exception("USB controller delivery failed: " + err);
                Bridge.PadFlowLog.Sent(slot, packet);
            }
            finally { inputWrites.Release(); }
        }

        // Operation 5 for one slot: the console detaches that slot's virtual pad (a remote viewer left).
        public async Task ReleaseSlot(int slot)
        {
            if (slot < 1 || slot > ExtraSlotCapacity) throw new ArgumentOutOfRangeException(nameof(slot));
            if (!SupportsInput || closed || Cancellation.IsCancellationRequested) return;
            await inputWrites.WaitAsync(Cancellation).ConfigureAwait(false);
            try
            {
                if (closed || Cancellation.IsCancellationRequested) return;
                var frame = ReleaseFrame(++inputSequence, inputSession, (byte)slot);
                var (err, transfer) = await writer.WriteAsync(frame, 500).ConfigureAwait(false);
                if (err != LibUsbDotNet.Error.Success || transfer != frame.Length)
                    throw new Exception("USB controller release failed: " + err);
                Bridge.PadFlowLog.Event("release", slot);
            }
            finally { inputWrites.Release(); }
        }

        internal static byte[] ReleaseFrame(uint sequence, ulong session, byte slot)
        {
            var frame = new byte[64];
            BinaryPrimitives.WriteUInt32LittleEndian(frame, 0x3143584E);
            BinaryPrimitives.WriteUInt16LittleEndian(frame.AsSpan(4), 1);
            BinaryPrimitives.WriteUInt32LittleEndian(frame.AsSpan(8), sequence);
            frame[12] = 5;
            frame[14] = slot;
            BinaryPrimitives.WriteUInt64LittleEndian(frame.AsSpan(16), session);
            return frame;
        }

        // The existing capture reader consumes owner replies between media packets. Call only
        // while that reader is running; never open a second USB IN reader for authorization.
        public async Task<byte[]> RequestOwnerProof(byte[] canonicalChallenge, CancellationToken cancel)
        {
            if (canonicalChallenge is null || canonicalChallenge.Length is < 1 or > UsbOwnerProtocol.MaximumRequest)
                throw new ArgumentException("Invalid USB owner challenge length.");
            if (!SupportsOwnerProof || closed) throw new InvalidOperationException("USB owner signer unavailable.");
            using var deadline = CancellationTokenSource.CreateLinkedTokenSource(Cancellation, cancel);
            deadline.CancelAfter(TimeSpan.FromSeconds(3));
            await ownerRequests.WaitAsync(deadline.Token).ConfigureAwait(false);
            var completion = new TaskCompletionSource<byte[]>(TaskCreationOptions.RunContinuationsAsynchronously);
            uint token;
            try
            {
                lock (ownerGate)
                {
                    if (closed || !SupportsOwnerProof) throw new InvalidOperationException("USB owner signer disconnected.");
                    token = ++ownerToken; if (token == 0) token = ++ownerToken;
                    pendingOwner = completion;
                    pendingReplyMagic = UsbOwnerProtocol.ResponseMagic;
                }
                await inputWrites.WaitAsync(deadline.Token).ConfigureAwait(false);
                try
                {
                    for (int offset = 0; offset < canonicalChallenge.Length; offset += 32)
                    {
                        deadline.Token.ThrowIfCancellationRequested();
                        var frame = UsbOwnerProtocol.Fragment(canonicalChallenge, token, offset, ++inputSequence, inputSession);
                        var (err, count) = await writer.WriteAsync(frame, 500).ConfigureAwait(false);
                        if (err != LibUsbDotNet.Error.Success || count != frame.Length) throw new Exception("USB owner request transfer failed.");
                    }
                }
                finally { inputWrites.Release(); }
                return await completion.Task.WaitAsync(deadline.Token).ConfigureAwait(false);
            }
            finally
            {
                lock (ownerGate) if (ReferenceEquals(pendingOwner, completion)) pendingOwner = null;
                ownerRequests.Release();
            }
        }
        public async Task RequestDelegation(byte operation, ulong generation, uint milliseconds, CancellationToken cancel)
        {
            if (!SupportsDelegation || closed) throw new InvalidOperationException("USB acknowledged delegation unavailable.");
            using var deadline = CancellationTokenSource.CreateLinkedTokenSource(Cancellation, cancel); deadline.CancelAfter(5000);
            await ownerRequests.WaitAsync(deadline.Token).ConfigureAwait(false);
            var completion = new TaskCompletionSource<byte[]>(TaskCreationOptions.RunContinuationsAsynchronously);
            try
            {
                uint token;
                lock (ownerGate)
                {
                    if (closed || !SupportsDelegation) throw new InvalidOperationException("USB delegation disconnected.");
                    token = ++ownerToken; if (token == 0) token = ++ownerToken; pendingOwner = completion; pendingReplyMagic = UsbDelegationProtocol.ResponseMagic;
                }
                await inputWrites.WaitAsync(deadline.Token).ConfigureAwait(false);
                try
                {
                    var frame = UsbDelegationProtocol.Frame(operation, generation, milliseconds, token, ++inputSequence, inputSession);
                    var (error, count) = await writer.WriteAsync(frame, 500).ConfigureAwait(false);
                    if (error != LibUsbDotNet.Error.Success || count != frame.Length) throw new Exception("USB delegation transfer failed.");
                }
                finally { inputWrites.Release(); }
                UsbDelegationProtocol.ValidateAcknowledgement(await completion.Task.WaitAsync(deadline.Token).ConfigureAwait(false), operation, generation, milliseconds);
            }
            finally
            {
                lock (ownerGate) if (ReferenceEquals(pendingOwner, completion)) pendingOwner = null;
                ownerRequests.Release();
            }
        }
        // ---- first-use owner-key setup (operations 8 and 9) ----
        // The response token is the control frame's sequence number, so nothing else can satisfy it.
        async Task<byte[]> EnrollmentExchange(Func<uint, byte[]> frameFor, CancellationToken cancel)
        {
            if (!enrollmentNegotiated || closed) throw new InvalidOperationException("USB key setup unavailable.");
            using var deadline = CancellationTokenSource.CreateLinkedTokenSource(Cancellation, cancel);
            deadline.CancelAfter(5000);
            await ownerRequests.WaitAsync(deadline.Token).ConfigureAwait(false);
            var completion = new TaskCompletionSource<byte[]>(TaskCreationOptions.RunContinuationsAsynchronously);
            try
            {
                await inputWrites.WaitAsync(deadline.Token).ConfigureAwait(false);
                try
                {
                    uint sequence = ++inputSequence; if (sequence == 0) sequence = ++inputSequence;
                    lock (ownerGate)
                    {
                        if (closed || !enrollmentNegotiated) throw new InvalidOperationException("USB key setup disconnected.");
                        ownerToken = sequence; pendingOwner = completion; pendingReplyMagic = UsbEnrollmentProtocol.ResponseMagic;
                    }
                    var frame = frameFor(sequence);
                    var (err, count) = await writer.WriteAsync(frame, 500).ConfigureAwait(false);
                    Array.Clear(frame); // the stage frame carries the key; do not leave a copy in memory
                    if (err != LibUsbDotNet.Error.Success || count != frame.Length) throw new Exception("USB key setup transfer failed.");
                }
                finally { inputWrites.Release(); }
                return await completion.Task.WaitAsync(deadline.Token).ConfigureAwait(false);
            }
            finally
            {
                lock (ownerGate) if (ReferenceEquals(pendingOwner, completion)) pendingOwner = null;
                ownerRequests.Release();
            }
        }

        public Task<byte[]> StageOwnerKey(ReadOnlyMemory<byte> key, CancellationToken cancel) =>
            EnrollmentExchange(sequence => UsbEnrollmentProtocol.StageFrame(SessionId, key.Span, sequence, inputSession), cancel);

        // On success the console is a full owner-signing host for this attachment: no reconnect needed.
        public async Task<byte[]> CommitOwnerKey(CancellationToken cancel)
        {
            var fingerprint = await EnrollmentExchange(sequence => UsbEnrollmentProtocol.CommitFrame(SessionId, sequence, inputSession), cancel).ConfigureAwait(false);
            lock (ownerGate)
            {
                if (closed) throw new InvalidOperationException("USB key setup disconnected.");
                SupportsOwnerProof = true;
                SupportsDelegation = true;
                CanEnroll = false;
            }
            return fingerprint;
        }

        void ResetOwnerProof()
        {
            lock (ownerGate)
            {
                SupportsOwnerProof = false;
                SupportsDelegation = false;
                CanEnroll = false;
                enrollmentNegotiated = false;
                pendingOwner?.TrySetCanceled(); pendingOwner = null;
            }
        }

        public override void Dispose()
        {
            if (closed)
                return;

            closed = true;
            SupportsInput = false;
            ResetOwnerProof();
            SessionId = SessionPassword = "";
            device.Close();
            device.Dispose();
            lock (transferTraceGate) transferTrace?.Dispose();
        }
    }
}
