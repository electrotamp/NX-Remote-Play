using NxRemotePlay.Client.Sources;
using System;
using System.Buffers.Binary;
using System.Diagnostics;
using System.Threading;
using System.Threading.Tasks;

namespace NxRemotePlay.Client.Bridge;

// One virtual Switch controller: an explicit host grant transfers it to one authenticated
// participant. Generation fencing prevents packets from an old grant from resuming input.
// The coordinator must revoke before disposing the USB source. Never merge two pads.
internal sealed class BridgeInputArbiter : IAsyncDisposable, IInputSink
{
    readonly IInputSink target;
    readonly SemaphoreSlim writes = new(1);
    readonly CancellationTokenSource lifetime = new();
    readonly Task watching;
    readonly Func<long> clock;
    readonly InputFlowLog? log;
    string? remote;
    ulong generation;
    uint sequence;
    bool sequenced, disposed;
    long lastInput;
    byte[] nativePad = PunchController.Packet(0, 0, 0, 0, 0);
    byte[] nativeTouch = PunchController.TouchPacket(false, 0, 0);
    bool nativeActive, nativeHome;
    bool localPadActive, localHomeActive, localTouchActive;
    long nativeRefresh;
    byte[] remotePad = PunchController.Packet(0, 0, 0, 0, 0);
    byte[] remoteTouch = PunchController.TouchPacket(false, 0, 0);
    byte remoteHome;
    long remoteRefresh;
    internal bool RemoteHasControl => Volatile.Read(ref remote) is not null;
    internal bool GrantedTo(string participant) => Volatile.Read(ref remote) == participant;
    public bool SupportsInput => !disposed && target.SupportsInput;

    // The arbiter owns `log` and disposes it with itself.
    internal BridgeInputArbiter(IInputSink target, Func<long>? clock = null, InputFlowLog? log = null)
    {
        this.target = target; this.clock = clock ?? (() => Environment.TickCount64); this.log = log;
        watching = Task.Run(Watch);
    }
    // A single USB OUT timeout (a transient stall) must not end a client's session or kill the watchdog:
    // pad state is re-sent every refresh/poll, so a lost write self-heals. Only a run of consecutive
    // failures (USB really gone) is surfaced to the caller. Always called under `writes`.
    const int MaxConsecutiveWriteFailures = 4;
    int consecutiveWriteFailures;
    async Task Out(byte[] packet)
    {
        long start = Stopwatch.GetTimestamp(); bool failed = true;
        try { await target.SendInput(packet); failed = false; consecutiveWriteFailures = 0; }
        catch (Exception error) when (error is not OperationCanceledException && ++consecutiveWriteFailures < MaxConsecutiveWriteFailures)
        { log?.Event("write_failed_tolerated", "consecutive=" + consecutiveWriteFailures); }
        finally { log?.Write(Stopwatch.GetElapsedTime(start).TotalMilliseconds, failed); }
    }
    // clearNative=false keeps what a native client is still physically holding, so it can be restored
    // when a remote owner releases the pad. Native state is cleared when that client disconnects.
    async Task Neutral(bool clearNative = true)
    {
        localPadActive = false; localHomeActive = false; localTouchActive = false;
        if (clearNative)
        {
            nativePad = PunchController.Packet(0, 0, 0, 0, 0); nativeHome = false;
            nativeTouch = PunchController.TouchPacket(false, 0, 0);
        }
        remotePad = PunchController.Packet(0, 0, 0, 0, 0); remoteHome = 0;
        remoteTouch = PunchController.TouchPacket(false, 0, 0); remoteRefresh = clock();
        await Out(PunchController.Packet(0, 0, 0, 0, 0));
        await Out(new byte[] { 3, 0, 0, 0, 0 });
        await Out(PunchController.TouchPacket(false, 0, 0));
    }
    internal async Task<ulong> Grant(string participant)
    {
        if (string.IsNullOrEmpty(participant) || participant.Length > 128) throw new ArgumentException("Invalid participant.");
        await writes.WaitAsync(lifetime.Token);
        try
        {
            if (disposed) throw new ObjectDisposedException(nameof(BridgeInputArbiter));
            remote = null; generation = checked(generation + 1); sequenced = false;
            await Neutral(false); lastInput = clock(); remote = participant;
            log?.Event("grant", "gen=" + generation);
            return generation;
        }
        finally { writes.Release(); }
    }
    internal async Task Revoke(string? participant = null)
    {
        await writes.WaitAsync(lifetime.Token);
        try
        {
            if (disposed || remote is null || participant is not null && remote != participant) return;
            remote = null; generation = checked(generation + 1); sequenced = false;
            log?.Event("revoke", "gen=" + generation);
            await Neutral(false); if (nativeActive) await RestoreNative();
        }
        finally { writes.Release(); }
    }
    public async Task SendInput(byte[] packet)
    {
        await writes.WaitAsync(lifetime.Token);
        try
        {
            if (disposed) return;
            if (remote is not null) { log?.Ignored(); return; }
            bool wasLocalActive = localPadActive || localHomeActive || localTouchActive;
            if (packet.Length == 17 && packet[0] == 2)
                localPadActive = BinaryPrimitives.ReadUInt64LittleEndian(packet.AsSpan(1)) != 0 ||
                    BinaryPrimitives.ReadInt16LittleEndian(packet.AsSpan(9)) != 0 || BinaryPrimitives.ReadInt16LittleEndian(packet.AsSpan(11)) != 0 ||
                    BinaryPrimitives.ReadInt16LittleEndian(packet.AsSpan(13)) != 0 || BinaryPrimitives.ReadInt16LittleEndian(packet.AsSpan(15)) != 0;
            else if (packet.Length == 5 && packet[0] == 3) localHomeActive = packet[1] != 0;
            else if (packet.Length == 5 && packet[0] <= 1) localTouchActive = packet[0] == 1;
            bool localActive = localPadActive || localHomeActive || localTouchActive;
            if (nativeActive)
            {
                // An idle desktop controller is polled every render frame. Its
                // neutral packets must not erase the receiving client's held pad.
                if (!localActive)
                {
                    if (wasLocalActive) await RestoreNative();
                    return;
                }
            }
            await Out(packet);
        }
        finally { writes.Release(); }
    }
    async Task RestoreNative()
    {
        await Out(nativePad);
        await Out(new byte[] { 3, nativeHome ? (byte)1 : (byte)0, 0, 0, 0 });
        await Out(nativeTouch);
        nativeRefresh = clock();
    }
    void RememberNativeState(byte[] packet)
    {
        if (packet.Length == 17 && packet[0] == 2) nativePad = (byte[])packet.Clone();
        else if (packet.Length == 5 && packet[0] == 3) nativeHome = packet[1] != 0;
        else if (packet.Length == 5 && packet[0] <= 1) nativeTouch = (byte[])packet.Clone();
        nativeRefresh = clock();
    }
    // Existing clients send the Switch's original controller/touch/HOME packets.
    // Browser grants still fence input, but native clients need no new negotiation.
    internal async Task<bool> NativeInput(byte[] packet)
    {
        bool valid = packet.Length == 17 && packet[0] == 2 && BinaryPrimitives.ReadUInt64LittleEndian(packet.AsSpan(1)) <= 65535 ||
            packet.Length == 5 && (packet[0] <= 1 && BinaryPrimitives.ReadUInt16LittleEndian(packet.AsSpan(1)) <= 1279 && BinaryPrimitives.ReadUInt16LittleEndian(packet.AsSpan(3)) <= 719 ||
            packet[0] == 3 && packet[1] <= 1 && packet[2] == 0 && packet[3] == 0 && packet[4] == 0);
        if (!valid) return false;
        await writes.WaitAsync(lifetime.Token);
        try
        {
            if (!disposed && !nativeActive) log?.Event("native_active");
            if (!disposed) nativeActive = true;
            if (!disposed && remote is null)
            {
                RememberNativeState(packet);
                if (!localPadActive && !localHomeActive && !localTouchActive) await Out(packet);
            }
            else if (!disposed) { RememberNativeState(packet); log?.Ignored(); }
            return true;
        }
        finally { writes.Release(); }
    }
    internal async Task NativeDisconnected()
    {
        await writes.WaitAsync(lifetime.Token);
        try
        {
            nativeActive = false; log?.Event("native_disconnected");
            if (!disposed && remote is null) await Neutral();
            else { nativePad = PunchController.Packet(0, 0, 0, 0, 0); nativeHome = false; nativeTouch = PunchController.TouchPacket(false, 0, 0); }
        }
        finally { writes.Release(); }
    }
    internal async Task<bool> RemoteSnapshot(string participant, byte[] bytes)
    {
        // Validate the whole snapshot before applying any component. The SCTP reader must
        // submit one at a time; a busy USB write drops new input instead of growing a queue.
        if (bytes.Length != 48 || BinaryPrimitives.ReadUInt32LittleEndian(bytes) != 0x3149584E ||
            BinaryPrimitives.ReadUInt16LittleEndian(bytes.AsSpan(4)) != 1 ||
            BinaryPrimitives.ReadUInt16LittleEndian(bytes.AsSpan(6)) != 48 || bytes[36] > 1 || bytes[37] > 1 ||
            BinaryPrimitives.ReadUInt16LittleEndian(bytes.AsSpan(38)) > 1279 ||
            BinaryPrimitives.ReadUInt16LittleEndian(bytes.AsSpan(40)) > 719 ||
            BinaryPrimitives.ReadUInt64LittleEndian(bytes.AsSpan(20)) > 65535) return false;
        for (int at = 42; at < 48; at++) if (bytes[at] != 0) return false;
        if (!await writes.WaitAsync(0, lifetime.Token)) { log?.BusyDrop(); return false; }
        try
        {
            ulong grant = BinaryPrimitives.ReadUInt64LittleEndian(bytes.AsSpan(8));
            uint next = BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(16));
            if (disposed || remote != participant || grant != generation || sequenced && unchecked((int)(next - sequence)) <= 0) return false;
            sequence = next; sequenced = true; lastInput = clock();
            var pad = PunchController.Packet(BinaryPrimitives.ReadUInt64LittleEndian(bytes.AsSpan(20)),
                BinaryPrimitives.ReadInt16LittleEndian(bytes.AsSpan(28)), BinaryPrimitives.ReadInt16LittleEndian(bytes.AsSpan(30)),
                BinaryPrimitives.ReadInt16LittleEndian(bytes.AsSpan(32)), BinaryPrimitives.ReadInt16LittleEndian(bytes.AsSpan(34)));
            var touch = PunchController.TouchPacket(bytes[37] != 0,
                BinaryPrimitives.ReadUInt16LittleEndian(bytes.AsSpan(38)), BinaryPrimitives.ReadUInt16LittleEndian(bytes.AsSpan(40)));
            bool refresh = clock() - remoteRefresh >= 250;
            if (refresh || !pad.AsSpan().SequenceEqual(remotePad)) { await Out(pad); remotePad = pad; }
            if (remoteHome != bytes[36] || refresh && remoteHome != 0) { await Out(new byte[] { 3, bytes[36], 0, 0, 0 }); remoteHome = bytes[36]; }
            if (!touch.AsSpan().SequenceEqual(remoteTouch) || refresh && remoteTouch[0] != 0) { await Out(touch); remoteTouch = touch; }
            if (refresh) remoteRefresh = clock();
            return true;
        }
        finally { writes.Release(); }
    }
    internal async Task CheckTimeout()
    {
        await writes.WaitAsync(lifetime.Token);
        try
        {
            if (!disposed && remote is not null && clock() - lastInput >= 500)
            {
                remote = null; generation = checked(generation + 1); sequenced = false;
                log?.Event("watchdog_revoke", "silent_ms=" + (clock() - lastInput));
                await Neutral(false); if (nativeActive) await RestoreNative();
            }
            // The USB input watchdog needs updates even when a legacy native client
            // deduplicates unchanged controller state. Refresh on the desktop only.
            if (!disposed && nativeActive && remote is null && !localPadActive && !localHomeActive && !localTouchActive && clock() - nativeRefresh >= 250)
            {
                await Out(nativePad);
                if (nativeHome) await Out(new byte[] { 3, 1, 0, 0, 0 });
                nativeRefresh = clock();
            }
        }
        finally { writes.Release(); }
    }
    async Task Watch()
    {
        try
        {
            while (true)
            {
                await Task.Delay(100, lifetime.Token);
                // A persistent USB failure is the media reader's job to surface; the watchdog must outlive it.
                try { await CheckTimeout(); } catch (Exception error) when (error is not OperationCanceledException) { log?.Event("watchdog_error", error.GetType().Name); }
                log?.Tick();
            }
        }
        catch (OperationCanceledException) when (lifetime.IsCancellationRequested) { }
    }
    public async ValueTask DisposeAsync()
    {
        lifetime.Cancel();
        try { await watching; }
        finally
        {
            await writes.WaitAsync();
            try { if (!disposed) { disposed = true; remote = null; try { await Neutral(); } catch (Exception) { } } }
            finally { writes.Release(); lifetime.Dispose(); log?.Dispose(); }
        }
    }
}
