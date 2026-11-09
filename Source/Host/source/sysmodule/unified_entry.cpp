/*
 * ams::Main / ams::init::InitializeSystemModule / ams::init::FinalizeSystemModule -
 * libstratosphere's own entrypoint hooks (its shim already owns __appInit/__appExit/main,
 * see main.c's notes), calling straight into the renamed TCP transport sysmodule logic. This
 * sequence is modeled on applet-mitm's own InitializeSystemModule/Startup/Main - the same
 * pattern that registered vi:u/vi:m and ran clean on .198 tonight - not a fresh guess.
 */
#include <stratosphere.hpp>
#include "unified_entry.h"

namespace ams
{

namespace init
{

void InitializeSystemModule()
{
    R_ABORT_UNLESS(sm::Initialize());
    fs::InitializeForSystem();
    fs::SetEnabledAutoAbort(false);
}

void FinalizeSystemModule()
{ /* ... */
}

namespace
{

// Same 2MB this sysmodule has used since the  turbojpeg/malloc fix -
// now the one heap both malloc() (via unified_malloc_bridge.cpp) and any C++
// allocation draw from, instead of two separate arenas.
constexpr size_t MallocBufferSize = 2 * 1024 * 1024;
alignas(os::MemoryPageSize) constinit u8 g_malloc_buffer[MallocBufferSize];

} // namespace

void Startup()
{
    init::InitializeAllocator(g_malloc_buffer, sizeof(g_malloc_buffer));
}

} // namespace init

void Main()
{
    os::SetThreadNamePointer(os::GetCurrentThread(), "nxrp.Main");

    NxRemotePlay_AppInit();
    NxRemotePlay_Main();    // blocks forever in IpcThread() during normal operation
    NxRemotePlay_AppExit(); // not reached in practice - kept for parity with the original sysmodule
}

} // namespace ams
