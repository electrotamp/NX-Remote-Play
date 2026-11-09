// SPDX-License-Identifier: GPL-2.0-only
// Adapted from hardware capture's validated Default-display source, not its encoder.
// These are raw pixels obtained THROUGH caps:sc, not direct framebuffer reads.
#include "core.h"
#include "nxrp_source.h"
#include "modes/UdpmVideo.h"
#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>

enum
{
    FrameBytes = 1280 * 720 * 4
};
static atomic_bool gateBusy;
static bool gateHealthy = true;
static bool sourceFailed;

bool NxrpGateBusy(void)
{
    return atomic_load_explicit(&gateBusy, memory_order_acquire);
}
u32 NxrpSourceState(void)
{
    return (!gateHealthy ? 2u : 0u) | (sourceFailed ? 4u : 0u);
}
bool NxrpSourceDone(void)
{
    return NxrpSourceState() != 0;
}
void NxrpSourceReset(void)
{
    // A failed restore is never cleared by reconnecting. Reboot/recovery required.
    if (!gateHealthy)
        return;
    sourceFailed = false;

}

static bool Gate(bool install)
{
    const u64 viProgram = 0x010000000000002DULL;
    const u64 offset = 0x11b470; // Lite23.0.1:  live-text/caller correlation
    const u32 original[2] = {0x39400400, 0xD65F03C0};
    const u32 enabled[2] = {0x52800020, 0xD65F03C0};
    const u32 *expected = install ? original : enabled;
    const u32 *desired = install ? enabled : original;
    u64 pid = 0, address = 0, scan = 0;
    Handle debug = INVALID_HANDLE;
    bool ok = false, attempted = false, rollbackOk = true;
    Result rc = pmdmntInitialize();
    if (R_FAILED(rc))
        return false;
    rc = pmdmntGetProcessId(&pid, viProgram);
    if (R_FAILED(rc))
    {
        pmdmntExit();
        return false;
    }
    atomic_store_explicit(&gateBusy, true, memory_order_release);
    rc = svcDebugActiveProcess(&debug, pid);
    if (R_FAILED(rc))
        goto done;
    // No filesystem/network logging while a foreign process is suspended.
    for (u32 i = 0; i < 512; ++i)
    {
        MemoryInfo mi = {0};
        u32 page = 0;
        rc = svcQueryDebugProcessMemory(&mi, &page, debug, scan);
        if (R_FAILED(rc) || !mi.size)
            break;
        if ((mi.type & 0xff) == MemType_CodeStatic && (mi.perm & Perm_X) && mi.size >= offset + 8)
        {
            u32 words[2] = {0};
            rc = svcReadDebugProcessMemory(words, debug, mi.addr + offset, sizeof(words));
            // Firmware23.0.1 qualification: accessor, padding, and next function.
            // Exact live-code identity/evidence is preserved in  diagnostics.
            const u32 contextExpected[10] = {0x00000000, 0x00000000, 0xb9400c08, 0x72001c3f, 0x52800069,
                                             0x1a880120, 0xd65f03c0, 0x00000000, 0x00000000, 0x00000000};
            u32 context[10] = {0};
            Result contextRc = svcReadDebugProcessMemory(context, debug, mi.addr + offset + 8, sizeof(context));
            if (mi.size == 0x180000 && R_SUCCEEDED(rc) && R_SUCCEEDED(contextRc) &&
                !memcmp(words, expected, sizeof(words)) && !memcmp(context, contextExpected, sizeof(context)))
            {
                address = mi.addr + offset;
                break;
            }
        }
        if (mi.addr + mi.size <= scan)
            break;
        scan = mi.addr + mi.size;
    }
    if (address)
    {
        u32 after[2] = {0};
        attempted = true;
        rc = svcWriteDebugProcessMemory(debug, (void *)desired, address, 8);
        Result verify = svcReadDebugProcessMemory(after, debug, address, 8);
        ok = R_SUCCEEDED(rc) && R_SUCCEEDED(verify) && !memcmp(after, desired, 8);
        if (!ok)
        {
            // On install failure, return to original; on restore failure, retry original.
            Result rollback = svcWriteDebugProcessMemory(debug, (void *)original, address, 8);
            verify = svcReadDebugProcessMemory(after, debug, address, 8);
            rollbackOk = R_SUCCEEDED(rollback) && R_SUCCEEDED(verify) && !memcmp(after, original, 8);
            if (!install && rollbackOk)
                ok = true;
        }
    }
done:
    if (debug != INVALID_HANDLE)
        svcCloseHandle(debug); // detach/resume on every path
    atomic_store_explicit(&gateBusy, false, memory_order_release);
    pmdmntExit();
    if ((!install && !ok) || (attempted && !rollbackOk))
        gateHealthy = false;
    return ok;
}

bool NxrpReadFrame(void *pixels, size_t capacity, uint64_t *elapsed_ns, uint64_t *capture_tick)
{
    if (!pixels || capacity < FrameBytes || !elapsed_ns || !capture_tick || NxrpSourceDone())
        return false;
    // Exact23.0.1 only; gate and neighboring function checked against  live code.
    if (hosversionGet() != MAKEHOSVERSION(23, 0, 1))
    {
        return false;
    }
    const u64 begin = svcGetSystemTick();
    bool caps = false, gate = false, opened = false, ok = false;
    enum
    {
        ChunkBytes = 256 * 1024
    };
    u8 *stage = aligned_alloc(0x1000, ChunkBytes);
    if (!stage)
    {
        sourceFailed = true;
        return false;
    }
    u32 phase = 0;
    u64 bytes = 0, width = 0, height = 0, read = 0;
    Result rc = capsscInitialize();
    if (R_FAILED(rc))
        goto cleanup;
    caps = true;
    phase = 1;
    gate = Gate(true);
    if (!gate)
        goto cleanup;
    phase = 2;
    rc = capsscOpenRawScreenShotReadStream(&bytes, &width, &height, ViLayerStack_Default, 1000000000ULL);
    if (R_FAILED(rc))
        goto cleanup;
    opened = true;
    if (bytes != FrameBytes || width != 1280 || height != 720)
        goto cleanup;
    phase = 3;
    // Normal IPC rejects device-shared mappings. Keep the service buffer in
    // ordinary heap memory, then copy into our owned GPU-visible destination.
    for (u64 done = 0; done < FrameBytes;)
    {
        const u64 want = FrameBytes - done < ChunkBytes ? FrameBytes - done : ChunkBytes;
        u64 got = 0;
        rc = capsscReadRawScreenShotReadStream(&got, stage, want, done);
        if (R_FAILED(rc) || got == 0 || got > want)
            break;
        memcpy((u8 *)pixels + done, stage, (size_t)got);
        done += got;
        read = done;
    }
    ok = R_SUCCEEDED(rc) && read == FrameBytes;
cleanup:
    if (opened)
    {
        Result close = capsscCloseRawScreenShotReadStream();
        ok = ok && R_SUCCEEDED(close);
    }
    if (gate)
    {
        const bool restored = Gate(false);
        ok = ok && restored;
    }
    if (caps)
        capsscExit();
    free(stage);
    *elapsed_ns = armTicksToNs(svcGetSystemTick() - begin);
    *capture_tick = begin; // acquisition begin, NOT an observed display-present timestamp
    if (!ok)
    {
        // capsscInitialize 0x615 is the single shared session being busy.
        // No gate was touched at phase zero. Keep it retryable so the worker
        // may hand over to a newly focused game. All later failures still park.
        const bool retryable = phase == 0 && rc == 0x615 && gateHealthy;
        sourceFailed = !retryable;
        return false;
    }
    armDCacheFlush(pixels, FrameBytes); // CPU-written owned source, before VIC reads
    return true;
}

void NxrpGameFrameCaptured(uint32_t skipped_presents)
{
    UdpmVideo_NoteCapture(skipped_presents);
}
