#pragma once
#include <switch.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "defines.h"
#include "../core.h"

extern atomic_bool IsThreadRunning;

// The only mode a release enters is USB (UsbAuto); leaving it runs ExitFn.
typedef struct
{
    void (*ExitFn)();
} StreamMode;

extern const StreamMode USB_MODE;
