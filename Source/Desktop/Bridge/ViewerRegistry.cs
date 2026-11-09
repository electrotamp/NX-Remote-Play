using NXRemotePlay.Protocol;
using NxRemotePlay.Client.Sources;
using System;
using System.Buffers.Binary;
using System.Threading;
using System.Threading.Tasks;

namespace NxRemotePlay.Client.Bridge;

// A console that accepts controller pads beyond slot 0 (multi-viewer firmware). Slot 0 stays the hosting
// desktop's own pad; remote viewers use slots 1..ExtraSlotCapacity, one virtual pad each.
internal interface ISlotInputSink : IInputSink
{
    int ExtraSlotCapacity { get; }
    Task SendInput(int slot, byte[] packet);
    // Detaches that slot's virtual pad (a viewer left). Never changes slot 0.
    Task ReleaseSlot(int slot);
}

// Admission and per-viewer controller routing for every remote viewer of one host, native and browser
// together (: 3 remote viewers, each with its own virtual controller).
//
// Each viewer holds a lease. With a slot-capable console the lease owns a private BridgeInputArbiter bound
// to its own USB slot, so the proven single-pad logic (generations, sequence fencing, watchdog, tolerance
// of a transient write failure) is reused unchanged per viewer and no viewer can touch another's pad.
// With an older console (one pad only) the first viewer keeps the old behaviour and shares the hosting
// desktop's pad through `hostPad`; later viewers are view-only. A lease with no controls is view-only.
internal sealed class ViewerRegistry : IAsyncDisposable
{
    readonly object gate = new();
    readonly IInputSink? source;
    readonly BridgeInputArbiter? hostPad;
    readonly int limit, extraSlots;
    readonly ViewerLease?[] active;
    int touchOwner = -1;
    bool disposed;
    internal int Count { get { lock (gate) { int n = 0; foreach (var lease in active) if (lease is not null) n++; return n; } } }
    internal bool Full => Count >= limit;
    // Some pad can be given to a viewer (a slot-capable console, or the host's shared pad on an older one).
    internal bool CanControl => extraSlots > 0 || hostPad is not null;

    internal ViewerRegistry(IInputSink? source, BridgeInputArbiter? hostPad, int limit = ViewerLimits.MaxRemoteViewers)
    {
        if (limit is < 1 or > 8) throw new ArgumentOutOfRangeException(nameof(limit));
        this.source = source; this.hostPad = hostPad; this.limit = limit;
        extraSlots = source is ISlotInputSink slots ? Math.Min(slots.ExtraSlotCapacity, limit) : 0;
        active = new ViewerLease?[limit];
    }

    // Null when every viewer place is taken.
    internal ViewerLease? TryAcquire()
    {
        lock (gate)
        {
            if (disposed) return null;
            for (int index = 0; index < limit; index++)
            {
                if (active[index] is not null) continue;
                BridgeInputArbiter? controls = null; SlotSink? sink = null; int slot = -1;
                if (index < extraSlots)
                {
                    slot = index + 1;
                    sink = new SlotSink((ISlotInputSink)source!, slot, this);
                    controls = new BridgeInputArbiter(sink, null, InputFlowLog.Open());
                }
                else if (extraSlots == 0 && index == 0 && hostPad is not null) { slot = 0; controls = hostPad; }
                PadFlowLog.Event("lease", slot, $"index={index} pad={(controls is null ? "none (view-only)" : slot == 0 ? "shared host pad" : "own slot")} extra_slots={extraSlots} limit={limit}");
                return active[index] = new ViewerLease(this, index, controls, slot, sink);
            }
            return null;
        }
    }

    internal async Task Release(ViewerLease lease)
    {
        // The slot stays reserved until its pad is detached, so a new viewer can never inherit stale input.
        PadFlowLog.Event("lease_end", lease.Slot, $"index={lease.Index}");
        try
        {
            if (lease.Sink is not null)
            {
                try { await lease.Controls!.DisposeAsync(); } catch (Exception) { }
                try { await lease.Sink.Release(); } catch (Exception) { }
            }
        }
        finally
        {
            lock (gate) { if (ReferenceEquals(active[lease.Index], lease)) active[lease.Index] = null; if (touchOwner == lease.Slot) touchOwner = -1; }
        }
    }

    // One touchscreen: the first viewer to press owns it until it releases (or leaves).
    internal bool Touch(int slot, bool pressed)
    {
        lock (gate)
        {
            if (pressed) { if (touchOwner is -1 || touchOwner == slot) { touchOwner = slot; return true; } return false; }
            if (touchOwner == slot) { touchOwner = -1; return true; }
            return false;
        }
    }

    internal void TouchReleased(int slot) { lock (gate) if (touchOwner == slot) touchOwner = -1; }

    public async ValueTask DisposeAsync()
    {
        ViewerLease?[] leases;
        lock (gate) { disposed = true; leases = (ViewerLease?[])active.Clone(); }
        foreach (var lease in leases) if (lease is not null) await lease.DisposeAsync();
    }

    // Wraps one USB slot for one viewer's arbiter. The viewer's pad attaches on its first real input (a non-neutral pad state or
    // HOME), so a viewer who only watches, or only touches the screen, never takes a player place; neutral pad refreshes are
    // dropped until then. The pad is detached when the lease ends.
    internal sealed class SlotSink(ISlotInputSink target, int slot, ViewerRegistry owner) : IInputSink
    {
        bool padAttached, anySent;
        public bool SupportsInput => target.SupportsInput;
        public async Task SendInput(byte[] packet)
        {
            bool pad = packet.Length == 17 && packet[0] == 2;
            bool home = packet.Length == 5 && packet[0] == 3;
            bool touch = packet.Length == 5 && packet[0] <= 1;
            if (!pad && !home && !touch) { PadFlowLog.Dropped(slot); return; }
            if (touch && !owner.Touch(slot, packet[0] == 1)) { PadFlowLog.Dropped(slot); return; }
            if (pad && !padAttached)
            {
                bool real = BinaryPrimitives.ReadUInt64LittleEndian(packet.AsSpan(1)) != 0 ||
                    BinaryPrimitives.ReadInt16LittleEndian(packet.AsSpan(9)) != 0 || BinaryPrimitives.ReadInt16LittleEndian(packet.AsSpan(11)) != 0 ||
                    BinaryPrimitives.ReadInt16LittleEndian(packet.AsSpan(13)) != 0 || BinaryPrimitives.ReadInt16LittleEndian(packet.AsSpan(15)) != 0;
                if (!real) { PadFlowLog.Dropped(slot); return; }
                padAttached = true;
                PadFlowLog.Event("first_pad_input", slot);
            }
            if (home && !padAttached)
            {
                if (packet[1] == 0) { PadFlowLog.Dropped(slot); return; }
                padAttached = true; // the console attaches this viewer's pad to press HOME
                PadFlowLog.Event("first_home_input", slot);
            }
            anySent = true;
            await target.SendInput(slot, packet);
        }
        internal async Task Release()
        {
            owner.TouchReleased(slot);
            if (!anySent) return;
            anySent = false; padAttached = false;
            await target.ReleaseSlot(slot);
        }
    }
}

internal sealed class ViewerLease : IAsyncDisposable
{
    readonly ViewerRegistry owner;
    int ended;
    internal readonly int Index;
    // Null: view-only. Never dispose it directly: the lease owns it (except the shared host pad).
    internal readonly BridgeInputArbiter? Controls;
    // 0 = shares the hosting desktop's pad (older console), 1..n = own pad, -1 = view-only.
    internal readonly int Slot;
    internal readonly ViewerRegistry.SlotSink? Sink;
    internal ViewerLease(ViewerRegistry owner, int index, BridgeInputArbiter? controls, int slot, ViewerRegistry.SlotSink? sink)
    { this.owner = owner; Index = index; Controls = controls; Slot = slot; Sink = sink; }
    public async ValueTask DisposeAsync()
    {
        if (Interlocked.Exchange(ref ended, 1) == 0) await owner.Release(this);
    }
}
