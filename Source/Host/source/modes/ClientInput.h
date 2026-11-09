#pragma once
#include "../core.h"
#define TOUCH_PACKET_SIZE 5
#define CTRL_PACKET_TYPE 0x02
#define CTRL_PACKET_SIZE 17
#define SYS_PACKET_TYPE 0x03
// One exclusive transport owns these handlers at a time, using the session gate.
//  (multi-viewer): slots 0..3, one virtual pad each. The un-suffixed calls act on slot 0 exactly as before.
bool ClientInputApplySlot(int slot, const u8 *packet, size_t size);
void ClientInputService(void);
void ClientInputReleaseSlot(int slot); // that slot's pad and HOME, and the touchscreen if it owns it
void ClientInputReleaseAll(void);      // every slot and the touchscreen
