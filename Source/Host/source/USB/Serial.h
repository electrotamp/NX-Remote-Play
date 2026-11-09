#pragma once
#include "UsbComms.h"

// NxRemotePlay specific usb code
Result UsbStreamingInitialize();


// This is not protected by a mutex, the caller should ensure thread safety
bool UsbStreamingSend(const void *data, size_t length);
