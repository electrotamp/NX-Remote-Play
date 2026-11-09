#pragma once

// Renamed from sysmodule/main.c's former __appInit/__appExit/main (libstratosphere's own
// runtime shim already owns those three symbol names) - see main.c's own notes at the top
// for why. unified_entry.cpp calls these from ams::init::InitializeSystemModule()/
// ams::Main()/ams::init::FinalizeSystemModule().

#ifdef __cplusplus
extern "C"
{
#endif

    void NxRemotePlay_AppInit(void);
    void NxRemotePlay_Main(void);
    void NxRemotePlay_AppExit(void);

#ifdef __cplusplus
}
#endif
