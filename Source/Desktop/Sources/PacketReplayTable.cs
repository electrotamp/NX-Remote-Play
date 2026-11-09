
using NxRemotePlay.Client.Core;
using System;

namespace NxRemotePlay.Client.Sources
{
    internal class PacketReplayTable : IDisposable
    {
        const int TableSize = 64;
        readonly PoolBuffer[] Table = new PoolBuffer[TableSize];


        public void AssignSlot(PoolBuffer block, int slot)
        {

            if (Table[slot] is not null)
            {
                Table[slot].Free();
            }

            Table[slot] = block;
            block.Reference();
        }

        public void Flush()
        {
            for (int i = 0; i < TableSize; i++)
            {
                Table[i]?.Free();
                Table[i] = null;
            }
        }

        public bool LookupSlot(int slot, out PoolBuffer? buffer)
        {
            // invaid slot
            if (slot == 0xFF)
            {
                buffer = null;
                return false;
            }

            buffer = Table[slot];
            buffer?.Reference();
            return buffer is not null;
        }

        public void Dispose()
        {
            Flush();
        }
    }
}
