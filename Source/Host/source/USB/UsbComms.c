#include <string.h>
#include <stdio.h>
#include "UsbComms.h"
#include "../modes/modes.h"
#include "UsbTransferWait.h"

#define TOTAL_INTERFACES 1

// Preserve the first endpoint failure, without media, pairing credentials or
// addresses. Error-only SD I/O avoids perturbing successful transfer timing.
static atomic_bool g_faultRecorded = false;
static void UsbTransferFault(const char *direction, const char *stage, Result rc, u32 urb,
                             const UsbDsReportData *report)
{
    if (atomic_exchange(&g_faultRecorded, true))
        return;
    char text[1024];
    int used = snprintf(text, sizeof(text), "direction=%s stage=%s rc=0x%x urb=%u tick=%llu\n", direction, stage, rc,
                        urb, (unsigned long long)svcGetSystemTick());
    if (report)
    {
        used += snprintf(text + used, sizeof(text) - used, "report_count=%u\n", report->report_count);
        for (u32 i = 0; i < report->report_count && i < 8 && used < (int)sizeof(text) - 120; ++i)
            used += snprintf(text + used, sizeof(text) - used, "entry=%u id=%u status=%u requested=%u transferred=%u\n",
                             i, report->report[i].id, report->report[i].urb_status, report->report[i].requestedSize,
                             report->report[i].transferredSize);
    }
    // This sysmodule intentionally does not mount fsdev; use native FS like its
    // existing configuration readers rather than stdio's sdmc device.
    FsFileSystem sd;
    if (R_FAILED(fsOpenSdCardFileSystem(&sd)))
    {
        atomic_store(&g_faultRecorded, false);
        return;
    }
    const char *path = "/config/nxremoteplay/usb_transfer_failure.txt";
    fsFsCreateFile(&sd, path, used, 0);
    FsFile file;
    Result result = fsFsOpenFile(&sd, path, FsOpenMode_Write, &file);
    if (R_SUCCEEDED(result))
    {
        result = fsFileSetSize(&file, used);
        if (R_SUCCEEDED(result))
            result = fsFileWrite(&file, 0, text, used, FsWriteOption_Flush);
        fsFileClose(&file);
    }
    fsFsClose(&sd);
    if (R_FAILED(result))
        atomic_store(&g_faultRecorded, false);
}

// USB remains enumerated while network modes reuse the StaticBuffers union.
// DMA storage must therefore live outside that union and its mode-switch memset.
static struct
{
    bool IsInUse;
    u8 alignas(0x1000) EndpointIn[0x1000];
    u8 alignas(0x1000) EndpointOut[0x1000];
} UsbEndpointBuffers;

typedef struct
{
    RwLock lock, lock_in, lock_out;
    bool initialized;
    bool fenced_in, fenced_out;

    UsbDsInterface *interface;
    UsbDsEndpoint *endpoint_in, *endpoint_out;

    u8 *endpoint_in_buffer, *endpoint_out_buffer;
} usbCommsInterface;

static bool g_usbSerialInitialized = false;

static usbCommsInterface g_usbCommsInterfaces[TOTAL_INTERFACES];

static RwLock g_usbCommsLock;

static Result _usbCommsInterfaceInit1x(u32 intf_ind, const UsbSerailInterfaceInfo *info);
static Result _usbCommsInterfaceInit5x(u32 intf_ind, const UsbSerailInterfaceInfo *info);
static Result _usbCommsInterfaceInit(u32 intf_ind, const UsbSerailInterfaceInfo *info);

static Result _usbSerialWrite(usbCommsInterface *interface, const void *buffer, size_t size, size_t *transferredSize,
                              u64 timeout);

static void _usbCommsUpdateInterfaceDescriptor(struct usb_interface_descriptor *desc,
                                               const UsbSerailInterfaceInfo *info)
{
    if (info != NULL)
    {
        desc->bInterfaceClass = info->bInterfaceClass;
        desc->bInterfaceSubClass = info->bInterfaceSubClass;
        desc->bInterfaceProtocol = info->bInterfaceProtocol;
    }
}

Result usbSerialInitialize(const UsbSerialInitializationInfo *info)
{
    Result rc = 0;
    rwlockWriteLock(&g_usbCommsLock);

    if (g_usbSerialInitialized)
    {
        rc = MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);
    }
    else if (info->NumInterfaces > TOTAL_INTERFACES)
    {
        rc = ERR_USB_OUT_OF_MEMORY;
    }
    else
    {
        rc = usbDsInitialize();

        if (R_SUCCEEDED(rc))
        {
            if (hosversionAtLeast(5, 0, 0))
            {
                u8 iManufacturer, iProduct, iSerialNumber;
                static const u16 supported_langs[1] = {0x0409};
                // Send language descriptor
                rc = usbDsAddUsbLanguageStringDescriptor(NULL, supported_langs, sizeof(supported_langs) / sizeof(u16));
                // Send manufacturer
                if (R_SUCCEEDED(rc))
                    rc = usbDsAddUsbStringDescriptor(&iManufacturer, info->DeviceManufacturer);
                // Send product
                if (R_SUCCEEDED(rc))
                    rc = usbDsAddUsbStringDescriptor(&iProduct, info->DeviceName);
                // Send serial number
                if (R_SUCCEEDED(rc))
                    rc = usbDsAddUsbStringDescriptor(&iSerialNumber, info->DeviceSerialNumber);

                // Send device descriptors
                struct usb_device_descriptor device_descriptor = {.bLength = USB_DT_DEVICE_SIZE,
                                                                  .bDescriptorType = USB_DT_DEVICE,
                                                                  .bcdUSB = 0x0110,
                                                                  .bDeviceClass = 0x00,
                                                                  .bDeviceSubClass = 0x00,
                                                                  .bDeviceProtocol = 0x00,
                                                                  .bMaxPacketSize0 = 0x40,
                                                                  .idVendor = info->VendorId,
                                                                  .idProduct = info->ProductId,
                                                                  .bcdDevice = 0x0100,
                                                                  .iManufacturer = iManufacturer,
                                                                  .iProduct = iProduct,
                                                                  .iSerialNumber = iSerialNumber,
                                                                  .bNumConfigurations = 0x01};
                // Full Speed is USB 1.1
                if (R_SUCCEEDED(rc))
                    rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Full, &device_descriptor);

                // High Speed is USB 2.0
                device_descriptor.bcdUSB = 0x0200;
                if (R_SUCCEEDED(rc))
                    rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_High, &device_descriptor);

                // Super Speed is USB 3.0
                device_descriptor.bcdUSB = 0x0300;
                // Upgrade packet size to 512
                device_descriptor.bMaxPacketSize0 = 0x09;
                if (R_SUCCEEDED(rc))
                    rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Super, &device_descriptor);

                // Define Binary Object Store
                u8 bos[0x16] = {0x05,       // .bLength
                                USB_DT_BOS, // .bDescriptorType
                                0x16, 0x00, // .wTotalLength
                                0x02,       // .bNumDeviceCaps

                                // USB 2.0
                                0x07,                     // .bLength
                                USB_DT_DEVICE_CAPABILITY, // .bDescriptorType
                                0x02,                     // .bDevCapabilityType
                                0x02, 0x00, 0x00, 0x00,   // dev_capability_data

                                // USB 3.0
                                0x0A,                     // .bLength
                                USB_DT_DEVICE_CAPABILITY, // .bDescriptorType
                                0x03,                     // .bDevCapabilityType
                                0x00, 0x0E, 0x00, 0x03, 0x00, 0x00, 0x00};
                if (R_SUCCEEDED(rc))
                    rc = usbDsSetBinaryObjectStore(bos, sizeof(bos));
            }

            if (R_SUCCEEDED(rc))
            {
                for (u32 i = 0; i < info->NumInterfaces; i++)
                {
                    usbCommsInterface *intf = &g_usbCommsInterfaces[i];
                    rwlockWriteLock(&intf->lock);
                    rwlockWriteLock(&intf->lock_in);
                    rwlockWriteLock(&intf->lock_out);
                    rc = _usbCommsInterfaceInit(i, &info->Interfaces[i]);
                    rwlockWriteUnlock(&intf->lock_out);
                    rwlockWriteUnlock(&intf->lock_in);
                    rwlockWriteUnlock(&intf->lock);
                    if (R_FAILED(rc))
                    {
                        break;
                    }
                }
            }
        }

        if (R_SUCCEEDED(rc) && hosversionAtLeast(5, 0, 0))
        {
            rc = usbDsEnable();
        }
    }

    if (R_SUCCEEDED(rc))
    {
        g_usbSerialInitialized = true;
    }

    rwlockWriteUnlock(&g_usbCommsLock);

    if (R_FAILED(rc))
    {
        usbSerialExit();
    }

    return rc;
}

static void _usbCommsInterfaceFree(usbCommsInterface *interface)
{
    rwlockWriteLock(&interface->lock);
    if (!interface->initialized)
    {
        rwlockWriteUnlock(&interface->lock);
        return;
    }

    rwlockWriteLock(&interface->lock_in);
    rwlockWriteLock(&interface->lock_out);

    interface->initialized = 0;

    interface->endpoint_in = NULL;
    interface->endpoint_out = NULL;
    interface->interface = NULL;

    UsbEndpointBuffers.IsInUse = false;
    interface->endpoint_in_buffer = NULL;
    interface->endpoint_out_buffer = NULL;

    rwlockWriteUnlock(&interface->lock_out);
    rwlockWriteUnlock(&interface->lock_in);

    rwlockWriteUnlock(&interface->lock);
}

void usbSerialExit(void)
{
    u32 i;

    rwlockWriteLock(&g_usbCommsLock);

    for (i = 0; i < TOTAL_INTERFACES; i++)
    {
        usbDsInterface_Close(g_usbCommsInterfaces[i].interface);
        _usbCommsInterfaceFree(&g_usbCommsInterfaces[i]);
    }

    usbDsExit();

    g_usbSerialInitialized = false;

    rwlockWriteUnlock(&g_usbCommsLock);
}

static Result _usbCommsInterfaceInit(u32 intf_ind, const UsbSerailInterfaceInfo *info)
{
    if (hosversionAtLeast(5, 0, 0))
    {
        return _usbCommsInterfaceInit5x(intf_ind, info);
    }
    else
    {
        return _usbCommsInterfaceInit1x(intf_ind, info);
    }
}

static Result _usbCommsInterfaceInit5x(u32 intf_ind, const UsbSerailInterfaceInfo *info)
{
    Result rc = 0;
    usbCommsInterface *interface = &g_usbCommsInterfaces[intf_ind];

    struct usb_interface_descriptor interface_descriptor = {
        .bLength = USB_DT_INTERFACE_SIZE,
        .bDescriptorType = USB_DT_INTERFACE,
        .bInterfaceNumber = 4,
        .bNumEndpoints = 2,
        .bInterfaceClass = USB_CLASS_VENDOR_SPEC,
        .bInterfaceSubClass = USB_CLASS_VENDOR_SPEC,
        .bInterfaceProtocol = USB_CLASS_VENDOR_SPEC,
    };
    _usbCommsUpdateInterfaceDescriptor(&interface_descriptor, info);

    struct usb_endpoint_descriptor endpoint_descriptor_in = {
        .bLength = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = USB_ENDPOINT_IN,
        .bmAttributes = USB_TRANSFER_TYPE_BULK,
        .wMaxPacketSize = 0x200,
    };

    struct usb_endpoint_descriptor endpoint_descriptor_out = {
        .bLength = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = USB_ENDPOINT_OUT,
        .bmAttributes = USB_TRANSFER_TYPE_BULK,
        .wMaxPacketSize = 0x40,
    };

    struct usb_ss_endpoint_companion_descriptor endpoint_companion = {
        .bLength = sizeof(struct usb_ss_endpoint_companion_descriptor),
        .bDescriptorType = USB_DT_SS_ENDPOINT_COMPANION,
        .bMaxBurst = 0x0F,
        .bmAttributes = 0x00,
        .wBytesPerInterval = 0x00,
    };

    interface->initialized = 1;
    interface->fenced_in = interface->fenced_out = false;

    // The buffer for PostBufferAsync commands must be 0x1000-byte aligned.
    // Since we have a single interface, statically allocate the endpoint buffer
    if (UsbEndpointBuffers.IsInUse)
        rc = ERR_USB_OUT_OF_MEMORY;
    else
    {
        UsbEndpointBuffers.IsInUse = true;
        interface->endpoint_in_buffer = UsbEndpointBuffers.EndpointIn;
        interface->endpoint_out_buffer = UsbEndpointBuffers.EndpointOut;
    }

    if (R_SUCCEEDED(rc))
    {
        memset(interface->endpoint_in_buffer, 0, 0x1000);
        memset(interface->endpoint_out_buffer, 0, 0x1000);
    }

    if (R_FAILED(rc))
        return rc;

    rc = usbDsRegisterInterface(&interface->interface);
    if (R_FAILED(rc))
        return rc;

    interface_descriptor.bInterfaceNumber = interface->interface->interface_index;
    endpoint_descriptor_in.bEndpointAddress += interface_descriptor.bInterfaceNumber + 1;
    endpoint_descriptor_out.bEndpointAddress += interface_descriptor.bInterfaceNumber + 1;

    // Full Speed Config
    rc = usbDsInterface_AppendConfigurationData(interface->interface, UsbDeviceSpeed_Full, &interface_descriptor,
                                                USB_DT_INTERFACE_SIZE);
    if (R_FAILED(rc))
        return rc;
    rc = usbDsInterface_AppendConfigurationData(interface->interface, UsbDeviceSpeed_Full, &endpoint_descriptor_in,
                                                USB_DT_ENDPOINT_SIZE);
    if (R_FAILED(rc))
        return rc;
    rc = usbDsInterface_AppendConfigurationData(interface->interface, UsbDeviceSpeed_Full, &endpoint_descriptor_out,
                                                USB_DT_ENDPOINT_SIZE);
    if (R_FAILED(rc))
        return rc;

    // High Speed Config
    endpoint_descriptor_in.wMaxPacketSize = 0x200;
    endpoint_descriptor_out.wMaxPacketSize = 0x200;
    rc = usbDsInterface_AppendConfigurationData(interface->interface, UsbDeviceSpeed_High, &interface_descriptor,
                                                USB_DT_INTERFACE_SIZE);
    if (R_FAILED(rc))
        return rc;
    rc = usbDsInterface_AppendConfigurationData(interface->interface, UsbDeviceSpeed_High, &endpoint_descriptor_in,
                                                USB_DT_ENDPOINT_SIZE);
    if (R_FAILED(rc))
        return rc;
    rc = usbDsInterface_AppendConfigurationData(interface->interface, UsbDeviceSpeed_High, &endpoint_descriptor_out,
                                                USB_DT_ENDPOINT_SIZE);
    if (R_FAILED(rc))
        return rc;

    // Super Speed Config
    endpoint_descriptor_in.wMaxPacketSize = 0x400;
    endpoint_descriptor_out.wMaxPacketSize = 0x400;
    rc = usbDsInterface_AppendConfigurationData(interface->interface, UsbDeviceSpeed_Super, &interface_descriptor,
                                                USB_DT_INTERFACE_SIZE);
    if (R_FAILED(rc))
        return rc;
    rc = usbDsInterface_AppendConfigurationData(interface->interface, UsbDeviceSpeed_Super, &endpoint_descriptor_in,
                                                USB_DT_ENDPOINT_SIZE);
    if (R_FAILED(rc))
        return rc;
    rc = usbDsInterface_AppendConfigurationData(interface->interface, UsbDeviceSpeed_Super, &endpoint_companion,
                                                USB_DT_SS_ENDPOINT_COMPANION_SIZE);
    if (R_FAILED(rc))
        return rc;
    rc = usbDsInterface_AppendConfigurationData(interface->interface, UsbDeviceSpeed_Super, &endpoint_descriptor_out,
                                                USB_DT_ENDPOINT_SIZE);
    if (R_FAILED(rc))
        return rc;
    rc = usbDsInterface_AppendConfigurationData(interface->interface, UsbDeviceSpeed_Super, &endpoint_companion,
                                                USB_DT_SS_ENDPOINT_COMPANION_SIZE);
    if (R_FAILED(rc))
        return rc;

    // Setup endpoints.
    rc = usbDsInterface_RegisterEndpoint(interface->interface, &interface->endpoint_in,
                                         endpoint_descriptor_in.bEndpointAddress);
    if (R_FAILED(rc))
        return rc;

    rc = usbDsInterface_RegisterEndpoint(interface->interface, &interface->endpoint_out,
                                         endpoint_descriptor_out.bEndpointAddress);
    if (R_FAILED(rc))
        return rc;

    rc = usbDsInterface_EnableInterface(interface->interface);
    if (R_FAILED(rc))
        return rc;

    return rc;
}

static Result _usbCommsInterfaceInit1x(u32 intf_ind, const UsbSerailInterfaceInfo *info)
{
    Result rc = 0;
    usbCommsInterface *interface = &g_usbCommsInterfaces[intf_ind];

    struct usb_interface_descriptor interface_descriptor = {
        .bLength = USB_DT_INTERFACE_SIZE,
        .bDescriptorType = USB_DT_INTERFACE,
        .bInterfaceNumber = intf_ind,
        .bInterfaceClass = USB_CLASS_VENDOR_SPEC,
        .bInterfaceSubClass = USB_CLASS_VENDOR_SPEC,
        .bInterfaceProtocol = USB_CLASS_VENDOR_SPEC,
    };
    _usbCommsUpdateInterfaceDescriptor(&interface_descriptor, info);

    struct usb_endpoint_descriptor endpoint_descriptor_in = {
        .bLength = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = USB_ENDPOINT_IN,
        .bmAttributes = USB_TRANSFER_TYPE_BULK,
        .wMaxPacketSize = 0x200,
    };

    struct usb_endpoint_descriptor endpoint_descriptor_out = {
        .bLength = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = USB_ENDPOINT_OUT,
        .bmAttributes = USB_TRANSFER_TYPE_BULK,
        .wMaxPacketSize = 0x200,
    };

    interface->initialized = 1;
    interface->fenced_in = interface->fenced_out = false;

    // The buffer for PostBufferAsync commands must be 0x1000-byte aligned.
    //  Since we have a single interface, statically allocate the endpoint buffer
    if (UsbEndpointBuffers.IsInUse)
        rc = ERR_USB_OUT_OF_MEMORY;
    else
    {
        UsbEndpointBuffers.IsInUse = true;
        interface->endpoint_in_buffer = UsbEndpointBuffers.EndpointIn;
        interface->endpoint_out_buffer = UsbEndpointBuffers.EndpointOut;
    }

    if (R_SUCCEEDED(rc))
    {
        memset(interface->endpoint_in_buffer, 0, 0x1000);
        memset(interface->endpoint_out_buffer, 0, 0x1000);
    }

    if (R_FAILED(rc))
        return rc;

    // Setup interface.
    rc = usbDsGetDsInterface(&interface->interface, &interface_descriptor, "usb");
    if (R_FAILED(rc))
        return rc;

    // Setup endpoints.
    rc = usbDsInterface_GetDsEndpoint(interface->interface, &interface->endpoint_in,
                                      &endpoint_descriptor_in); // device->host
    if (R_FAILED(rc))
        return rc;

    rc = usbDsInterface_GetDsEndpoint(interface->interface, &interface->endpoint_out,
                                      &endpoint_descriptor_out); // host->device
    if (R_FAILED(rc))
        return rc;

    rc = usbDsInterface_EnableInterface(interface->interface);
    if (R_FAILED(rc))
        return rc;

    return rc;
}

static Result _usbSerialRead(usbCommsInterface *interface, void *buffer, size_t size, size_t *transferredSize,
                             u64 timeout)
{
    Result rc = 0;
    u32 urbId = 0;
    u8 *bufptr = (u8 *)buffer;
    u8 *transfer_buffer = NULL;
    u32 chunksize = 0;
    u32 tmp_transferredSize = 0;
    size_t total_transferredSize = 0;
    UsbDsReportData reportdata;

    if (interface->fenced_out)
        return KernelError_TimedOut;
    // Makes sure endpoints are ready for data-transfer / wait for init if needed.
    rc = usbDsWaitReady(timeout);
    if (R_FAILED(rc))
        return rc;

    while (size)
    {
        // DMA only into retained, endpoint-owned memory, including aligned callers.
        // A failed cancellation must not retain a caller's stack/capture buffer.
        transfer_buffer = interface->endpoint_out_buffer;
        chunksize = size < 0x1000 ? size : 0x1000;
        memset(transfer_buffer, 0, 0x1000);

        // Start a host->device transfer.
        rc = usbDsEndpoint_PostBufferAsync(interface->endpoint_out, transfer_buffer, chunksize, &urbId);
        if (R_FAILED(rc))
            return rc;

        // Wait for the transfer to finish.
        rc = UsbTransferWait(interface->endpoint_out, timeout, &interface->fenced_out);
        if (R_FAILED(rc))
        {
            UsbTransferFault("OUT", "wait", rc, urbId, NULL);
            return rc;
        }

        rc = usbDsEndpoint_GetReportData(interface->endpoint_out, &reportdata);
        if (R_FAILED(rc))
        {
            interface->fenced_out = true;
            UsbTransferFault("OUT", "report", rc, urbId, NULL);
            return rc;
        }
        if (!UsbTransferReportComplete(&reportdata, urbId, &interface->fenced_out))
        {
            UsbTransferFault("OUT", "unconfirmed", KernelError_TimedOut, urbId, &reportdata);
            return KernelError_TimedOut;
        }

        rc = usbDsParseReportData(&reportdata, urbId, NULL, &tmp_transferredSize);
        if (R_FAILED(rc))
            return rc;

        if (tmp_transferredSize > chunksize)
            tmp_transferredSize = chunksize;
        total_transferredSize += (size_t)tmp_transferredSize;

        memcpy(bufptr, transfer_buffer, tmp_transferredSize);
        bufptr += tmp_transferredSize;
        size -= tmp_transferredSize;

        if (tmp_transferredSize < chunksize)
            break;
    }

    if (transferredSize)
        *transferredSize = total_transferredSize;

    return rc;
}

static Result _usbSerialWrite(usbCommsInterface *interface, const void *buffer, size_t size, size_t *transferredSize,
                              u64 timeout)
{
    Result rc = 0;
    u32 urbId = 0;
    u32 chunksize = 0;
    u8 *bufptr = (u8 *)buffer;
    u8 *transfer_buffer = NULL;
    u32 tmp_transferredSize = 0;
    size_t total_transferredSize = 0;
    UsbDsReportData reportdata;

    if (interface->fenced_in)
        return KernelError_TimedOut;
    // Makes sure endpoints are ready for data-transfer / wait for init if needed.
    rc = usbDsWaitReady(timeout);
    if (R_FAILED(rc))
        return rc;

    while (size)
    {
        transfer_buffer = interface->endpoint_in_buffer;
        chunksize = size < 0x1000 ? size : 0x1000;
        memcpy(transfer_buffer, bufptr, chunksize);

        // Start a device->host transfer.
        rc = usbDsEndpoint_PostBufferAsync(interface->endpoint_in, transfer_buffer, chunksize, &urbId);
        if (R_FAILED(rc))
            return rc;

        // Wait for the transfer to finish.
        rc = UsbTransferWait(interface->endpoint_in, timeout, &interface->fenced_in);
        if (R_FAILED(rc))
        {
            UsbTransferFault("IN", "wait", rc, urbId, NULL);
            return rc;
        }

        rc = usbDsEndpoint_GetReportData(interface->endpoint_in, &reportdata);
        if (R_FAILED(rc))
        {
            interface->fenced_in = true;
            UsbTransferFault("IN", "report", rc, urbId, NULL);
            return rc;
        }
        if (!UsbTransferReportComplete(&reportdata, urbId, &interface->fenced_in))
        {
            UsbTransferFault("IN", "unconfirmed", KernelError_TimedOut, urbId, &reportdata);
            return KernelError_TimedOut;
        }

        rc = usbDsParseReportData(&reportdata, urbId, NULL, &tmp_transferredSize);
        if (R_FAILED(rc))
            return rc;

        if (tmp_transferredSize > chunksize)
            tmp_transferredSize = chunksize;

        total_transferredSize += (size_t)tmp_transferredSize;

        bufptr += tmp_transferredSize;
        size -= tmp_transferredSize;

        if (tmp_transferredSize < chunksize)
            break;
    }

    if (transferredSize)
        *transferredSize = total_transferredSize;

    return rc;
}

size_t usbSerialReadEx(void *buffer, size_t size, u32 interface, u64 timeout)
{
    size_t transferredSize = 0;
    u32 state = 0;
    Result rc, rc2;
    usbCommsInterface *inter = &g_usbCommsInterfaces[interface];
    bool initialized;

    if (interface >= TOTAL_INTERFACES)
        return 0;

    rwlockReadLock(&inter->lock);
    initialized = inter->initialized;
    rwlockReadUnlock(&inter->lock);
    if (!initialized)
        return 0;

    rwlockWriteLock(&inter->lock_out);
    rc = _usbSerialRead(inter, buffer, size, &transferredSize, timeout);
    rwlockWriteUnlock(&inter->lock_out);
    if (R_FAILED(rc) && rc != KernelError_TimedOut)
    {
        rc2 = usbDsGetState(&state);
        if (R_SUCCEEDED(rc2))
        {
            if (state != 5)
            {
                rwlockWriteLock(&inter->lock_out);
                rc = _usbSerialRead(
                    &g_usbCommsInterfaces[interface], buffer, size, &transferredSize,
                    timeout); // If state changed during transfer, try again. usbDsWaitReady() will be called from this.
                rwlockWriteUnlock(&inter->lock_out);
            }
        }
    }
    return transferredSize;
}

size_t usbSerialRead(void *buffer, size_t size, u64 timeout)
{
    return usbSerialReadEx(buffer, size, 0, timeout);
}

size_t usbSerialWriteEx(const void *buffer, size_t size, u32 interface, u64 timeout)
{
    size_t transferredSize = 0;
    u32 state = 0;
    Result rc, rc2;
    usbCommsInterface *inter = &g_usbCommsInterfaces[interface];
    bool initialized;

    if (interface >= TOTAL_INTERFACES)
        return 0;

    rwlockReadLock(&inter->lock);
    initialized = inter->initialized;
    rwlockReadUnlock(&inter->lock);
    if (!initialized)
        return 0;

    rwlockWriteLock(&inter->lock_in);
    rc = _usbSerialWrite(&g_usbCommsInterfaces[interface], buffer, size, &transferredSize, timeout);
    rwlockWriteUnlock(&inter->lock_in);
    if (R_FAILED(rc) && rc != KernelError_TimedOut)
    {
        rc2 = usbDsGetState(&state);
        if (R_SUCCEEDED(rc2))
        {
            if (state != 5)
            {
                rwlockWriteLock(&inter->lock_in);
                rc = _usbSerialWrite(
                    &g_usbCommsInterfaces[interface], buffer, size, &transferredSize,
                    timeout); // If state changed during transfer, try again. usbDsWaitReady() will be called from this.
                rwlockWriteUnlock(&inter->lock_in);
            }
        }
    }
    return transferredSize;
}

size_t usbSerialWrite(const void *buffer, size_t size, u64 timeout)
{
    return usbSerialWriteEx(buffer, size, 0, timeout);
}
