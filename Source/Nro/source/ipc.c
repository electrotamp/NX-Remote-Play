#include <stdio.h>
#include <string.h>
#include "ipc.h"


static Service hostService;

// Ams extension, see https://github.com/Atmosphere-NX/Atmosphere/blob/master/docs/components/modules/sm.md#ipc-commands
static Result smAtmosphereHasService(SmServiceName name, bool *available)
{
    // _smShouldUseTipc from libnx
    // Not sure if this check is actually needed since this is supposed to work only on ams anyway
    if (hosversionIsAtmosphere() || hosversionAtLeast(12, 0, 0))
        return tipcDispatchInOut(smGetServiceSessionTipc(), 65100, name, *available);
    else
        return serviceDispatchInOut(smGetServiceSession(), 65100, name, *available);
}

static bool smLegacyServiceCheck(SmServiceName name)
{
    Handle h;
    Result rc = smRegisterService(&h, name, false, 1);

    if (R_SUCCEEDED(rc))
    {
        smUnregisterService(name);
        return false;
    }

    return true;
}

bool NxRemotePlayIsRunning()
{
    SmServiceName name = {0};
    memcpy(name.name, "nxremote", sizeof(name.name)); // exactly 8 characters: the Horizon service name limit

    bool available = false;
    Result rc = smAtmosphereHasService(name, &available);
    if (R_SUCCEEDED(rc))
    {
        printf("Using modern service check\n");
        return available;
    }

    // Hopefully, never used ever again.
    printf("Modern check failed with %x, using legacy check...\n", rc);
    return smLegacyServiceCheck(name);
}

Result NxRemotePlayConnect()
{
    // If we're running the USB version the process will be counted as running but there will be no IPC service
    // This is an issue since smGetService block forever if the service name is not found. We must always ensure it is
    // available first.
    if (!NxRemotePlayIsRunning())
        return ERR_MAIN_NOTRUNNING;

    Result rc = smGetService(&hostService, "nxremote");
    return rc;
}

void NxRemotePlayClose()
{
    serviceClose(&hostService);
}

Result NxRemotePlayGetHostingState(HostingSnapshot *out)
{
    HostingSnapshot snapshot;
    Result rc = serviceDispatchOut(&hostService, CMD_GET_HOSTING_STATE, snapshot);
    if (R_SUCCEEDED(rc))
        *out = snapshot;
    return rc;
}

Result NxRemotePlayRequestNewCode()
{
    return serviceDispatch(&hostService, CMD_REQUEST_NEW_CODE);
}

Result NxRemotePlaySetHostingEnabled(bool enabled)
{
    u32 val = enabled ? 1 : 0;
    return serviceDispatchIn(&hostService, CMD_SET_HOSTING_ENABLED, val);
}

Result NxRemotePlayGetServer(char out[64])
{
    char buf[64] = {0};
    Result rc = serviceDispatchOut(&hostService, CMD_GET_SERVER, buf);
    if (R_SUCCEEDED(rc))
    {
        memcpy(out, buf, sizeof(buf));
        out[63] = 0;
    }
    return rc;
}

Result NxRemotePlaySetServer(const char in[64])
{
    char buf[64] = {0};
    memcpy(buf, in, sizeof(buf));
    buf[63] = 0;
    return serviceDispatchIn(&hostService, CMD_SET_SERVER, buf);
}
