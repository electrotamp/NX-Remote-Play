#include <string.h>

#include "ipc.h"
#include "../modes/defines.h"
#include "../modes/PunchWake.h"
#include "../core.h"
#include "../util.h"

static Handle handles[2];
static SmServiceName serverName;

static Handle *const serverHandle = &handles[0];
static Handle *const clientHandle = &handles[1];

static void StartServer()
{
    *serverHandle = INVALID_HANDLE;
    *clientHandle = INVALID_HANDLE;

    memcpy(serverName.name, "nxremote",
           sizeof(serverName.name)); // exactly 8 characters: the Horizon service name limit
    R_THROW(smRegisterService(serverHandle, serverName, false, 1));
}

static void DisconnectClient()
{
    if (*clientHandle != INVALID_HANDLE)
    {
        svcCloseHandle(*clientHandle);
        *clientHandle = INVALID_HANDLE;
    }
}

static void StopServer()
{
    DisconnectClient();
    svcCloseHandle(*serverHandle);
    smUnregisterService(serverName);
    *serverHandle = INVALID_HANDLE;
}

// dataBuf's size just needs to cover the largest input payload any command
// here actually uses (currently a single u32, 4 bytes) - 32 bytes is
// generous headroom for anything foreseeable, not a real constraint.
#define REQUEST_DATA_BUF_SIZE 96

typedef struct
{
    u32 type;
    u64 cmdId;
    u32 dataSize;
    u8 dataBuf[REQUEST_DATA_BUF_SIZE];
} Request;

static Request ParseRequestFromTLS()
{
    Request req = {0};

    void *base = armGetTls();
    HipcParsedRequest hipc = hipcParseRequest(base);

    req.type = hipc.meta.type;

    if (hipc.meta.type == CmifCommandType_Request)
    {
        CmifInHeader *header = (CmifInHeader *)cmifGetAlignedDataStart(hipc.data.data_words, base);
        size_t dataSize = hipc.meta.num_data_words * 4;

        if (!header)
            fatalThrow(ERR_IPC_INVHEADER);
        if (dataSize < sizeof(CmifInHeader))
            fatalThrow(ERR_IPC_INVSIZE);
        if (header->magic != CMIF_IN_HEADER_MAGIC)
            fatalThrow(ERR_IPC_INVMAGIC);

        req.cmdId = header->command_id;
        req.dataSize = dataSize - sizeof(CmifInHeader);

        // Copy before later service calls can overwrite the thread-local IPC buffer.
        if (req.dataSize)
        {
            size_t copySize = req.dataSize < REQUEST_DATA_BUF_SIZE ? req.dataSize : REQUEST_DATA_BUF_SIZE;
            memcpy(req.dataBuf, ((u8 *)header) + sizeof(CmifInHeader), copySize);
        }
    }

    return req;
}

static void WriteResponseToTLS(Result rc)
{
    HipcMetadata meta = {0};
    meta.type = CmifCommandType_Request;
    meta.num_data_words = (sizeof(CmifOutHeader) + 0x10) / 4;

    void *base = armGetTls();
    HipcRequest hipc = hipcMakeRequest(base, meta);
    CmifOutHeader *rawHeader = (CmifOutHeader *)cmifGetAlignedDataStart(hipc.data_words, base);

    rawHeader->magic = CMIF_OUT_HEADER_MAGIC;
    rawHeader->result = rc;
    rawHeader->token = 0;
}

static bool ReadPayload(const Request *req, void *data, u32 len)
{
    if (req->dataSize < len || len > sizeof(req->dataBuf))
        return false;

    memcpy(data, req->dataBuf, len);
    return true;
}

static void WritePayloadResponseToTLS(Result rc, const void *payload, u32 len)
{
    HipcMetadata meta = {0};
    meta.type = CmifCommandType_Request;
    meta.num_data_words = (sizeof(CmifOutHeader) + 0x10 + len) / 4;

    void *base = armGetTls();
    HipcRequest hipc = hipcMakeRequest(base, meta);
    CmifOutHeader *rawHeader = (CmifOutHeader *)cmifGetAlignedDataStart(hipc.data_words, base);

    rawHeader->magic = CMIF_OUT_HEADER_MAGIC;
    rawHeader->result = rc;
    rawHeader->token = 0;

    memcpy(((u8 *)rawHeader) + sizeof(CmifOutHeader), payload, len);
}

static bool HandleCommand(const Request *req)
{
    switch (req->cmdId)
    {
    case CMD_GET_HOSTING_STATE: {
        HostingSnapshot snapshot;
        PunchWake_GetSnapshot(&snapshot);
        WritePayloadResponseToTLS(0, &snapshot, sizeof(snapshot));
        return false;
    }
    case CMD_REQUEST_NEW_CODE: {
        PunchWake_RequestNewCode();
        WriteResponseToTLS(0);
        return false;
    }
    case CMD_SET_HOSTING_ENABLED: {
        u32 enabled = 0;
        if (!ReadPayload(req, &enabled, sizeof(enabled)))
        {
            WriteResponseToTLS(ERR_IPC_INVSIZE);
            return false;
        }
        PunchWake_SetEnabled(enabled != 0);
        WriteResponseToTLS(0);
        return false;
    }
    case CMD_GET_SERVER: {
        char server[64] = {0};
        PunchWake_GetServer(server, sizeof(server));
        WritePayloadResponseToTLS(0, server, sizeof(server));
        return false;
    }
    case CMD_SET_SERVER: {
        char server[64] = {0};
        if (!ReadPayload(req, server, sizeof(server)))
        {
            WriteResponseToTLS(ERR_IPC_INVSIZE);
            return false;
        }
        server[sizeof(server) - 1] = 0;
        // Nonzero result = rejected (malformed host:port); nothing was changed.
        WriteResponseToTLS(PunchWake_SetServer(server) ? 0 : ERR_IPC_INVSIZE);
        return false;
    }
    default:
        WriteResponseToTLS(ERR_IPC_UNKCMD);
        return true;
    }
}

static bool IsClientConnected()
{
    return *clientHandle != INVALID_HANDLE;
}

typedef enum
{
    // Command handled, no error
    IPC_OK,
    // Non fatal error
    IPC_AGAIN,
    // Request to terminate IPC server
    IPC_TERMINATE
} IpcStatus;

static IpcStatus WaitAndProcessRequest()
{
    s32 index = -1;
    Result rc = svcWaitSynchronization(&index, handles, IsClientConnected() ? 2 : 1, UINT64_MAX);

    // Handle common errors
    if (R_FAILED(rc))
    {
        // Note: we currently don't use cancellation, but it's best to avoid throwing fatal here in case the OS or some
        // other process tries to mess with us
        if (rc == KERNELRESULT(ThreadTerminating) || rc == KERNELRESULT(Cancelled))
        {
            return IPC_TERMINATE;
        }
        else if (rc == KERNELRESULT(InvalidHandle))
        {
            // The client might have disconnected unexpectedly
            if (IsClientConnected())
            {
                DisconnectClient();
                return IPC_AGAIN;
            }
            else
            {
                // Our server handle went bad, terminate the server. Let's avoid crashing the console.
                return IPC_TERMINATE;
            }
        }

        // Other errors are fatal
        fatalThrow(rc);
    }

    if (index == 0)
    {
        Handle newcli;
        // Accept session
        if (R_FAILED(svcAcceptSession(&newcli, *serverHandle)))
            return IPC_AGAIN;

        // Max clients reached
        if (IsClientConnected())
        {
            svcCloseHandle(newcli);
            return IPC_AGAIN;
        }


        *clientHandle = newcli;
        return IPC_OK;
    }
    else if (index == 1)
    {
        if (!IsClientConnected())
        {
            // This should never happen
            return IPC_AGAIN;
        }

        // Receive the request
        s32 _idx;
        rc = svcReplyAndReceive(&_idx, clientHandle, 1, 0, UINT64_MAX);
        if (R_FAILED(rc))
        {
            DisconnectClient();
            return IPC_AGAIN;
        }

        // Decode and handle the message
        bool shouldClose = false;
        Request r = ParseRequestFromTLS();
        switch (r.type)
        {
        case CmifCommandType_Request:
            shouldClose = HandleCommand(&r);
            break;
        case CmifCommandType_Close:
            WriteResponseToTLS(0);
            shouldClose = true;
            break;
        default:
            WriteResponseToTLS(ERR_HIPC_UNKREQ);
            break;
        }

        // Finally, send the response.
        rc = svcReplyAndReceive(&_idx, NULL, 0, *clientHandle, 0);
        if (R_FAILED(rc) && rc != KERNELRESULT(TimedOut))
        {
            DisconnectClient();
            return IPC_AGAIN;
        }

        if (shouldClose)
            DisconnectClient();

        return IPC_OK;
    }

    return IPC_AGAIN;
}

void IpcThread()
{
    StartServer();

    while (1)
    {
        IpcStatus status = WaitAndProcessRequest();
        if (status == IPC_TERMINATE)
            break;
    }

    StopServer();
}
