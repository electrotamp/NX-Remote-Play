#pragma once

#include <switch.h>

#ifdef __cplusplus
extern "C"
{
#endif
#include "HostProtocol/defines.h"
#include "HostProtocol/HostingSnapshot.h"

    // Live hosting/configuration IPC; service availability is checked before connecting.
    Result NxRemotePlayConnect();
    void NxRemotePlayClose();

    // Session-hosting IPC - see
    // sysmodule/source/modes/SessionHosting.h. NxRemotePlayGetHostingState reads
    // the sysmodule's live, real Session ID/Password (the "connect anytime"
    // feature, on by default) instead of the NRO doing its own independent
    // REGISTER networking. NxRemotePlayRequestNewCode is the manual-refresh button.
    Result NxRemotePlayGetHostingState(HostingSnapshot *out);
    Result NxRemotePlayRequestNewCode();

    // Turns the sysmodule's "connect anytime" background feature fully
    // on/off (Settings screen toggle, ) - persisted across
    // reboots on the real sysmodule; a preview-only in-memory flag in the
    // non-Switch stub build.
    Result NxRemotePlaySetHostingEnabled(bool enabled);

    // Rendezvous server ("host:port", 64-byte NUL-terminated buffer) the sysmodule registers
    // with. NxRemotePlaySetServer fails (nonzero) if the sysmodule rejects the
    // value (malformed host or port); on success it persists it and re-registers.
    Result NxRemotePlayGetServer(char out[64]);
    Result NxRemotePlaySetServer(const char in[64]);
#ifdef __cplusplus
}
#endif
