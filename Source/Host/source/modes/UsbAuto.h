#pragma once
#include "../core.h"
// Enumerate USB independently of the selected network mode. Capture starts
// only after a client sends a valid NxRemotePlay request and claims the session gate.
void UsbAuto_Start(void);
