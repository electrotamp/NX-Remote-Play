#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#define USB_CAPABILITY_MARKER 0x3155534Eu
#define USB_CONTROL_MAGIC 0x3143584Eu
#define USB_CAP_IDENTITY 1u
#define USB_CAP_INPUT 2u
#define USB_CAP_INPUT_TIMEOUT 4u
#define USB_CAP_OWNER_SIGNER 8u
#define USB_CAP_OWNER_DELEGATION 16u
// First-use owner-key setup is available: no usable key yet, and operations 8 (stage) / 9 (commit) are accepted.
#define USB_CAP_OWNER_ENROLL 32u
// Multi-viewer : the console also accepts controller frames for slots 1..n (one virtual pad per
// remote viewer), n in capability byte 13. Slot 0 stays the hosting desktop's pad, exactly as before. The
// legacy slot-capacity byte (12) stays 1 so desktops that predate this keep validating the record.
#define USB_CAP_VIEWER_SLOTS 64u
#define USB_VIEWER_SLOTS 3
typedef struct __attribute__((packed))
{
    uint32_t magic;
    uint16_t version, size;
    uint32_t capabilities;
    uint8_t slots, reserved[3];
    char id[32], password[64], server[64], serial[32];
    uint64_t session;
    uint8_t padding[40];
} UsbCapabilities;
_Static_assert(sizeof(UsbCapabilities) == 256, "USB capabilities size");
typedef struct __attribute__((packed))
{
    uint32_t magic;
    uint16_t version, length;
    uint32_t sequence;
    uint8_t operation, source, slot, reserved;
    uint64_t session;
    uint8_t payload[40];
} UsbControlFrame;
_Static_assert(sizeof(UsbControlFrame) == 64, "USB command size");
// Slot 0 is the hosting desktop's pad; slots 1..USB_VIEWER_SLOTS are remote viewers' pads. Only the pad, touch, HOME,
// heartbeat and release operations are per slot; owner signing, delegation and enrollment stay slot 0.
static inline bool UsbControlValid(const UsbControlFrame *f)
{
    if (f->magic != USB_CONTROL_MAGIC || f->version != 1 || f->source != 0 || f->slot > USB_VIEWER_SLOTS ||
        f->reserved != 0 || f->session == 0)
        return false;
    if (f->slot != 0 && f->operation > 5)
        return false;
    switch (f->operation)
    {
    case 1:
        return f->length == 17 && f->payload[0] == 2;
    case 2:
        return f->length == 5 && f->payload[0] <= 1;
    case 3:
        return f->length == 5 && f->payload[0] == 3 && f->payload[1] <= 1 && f->payload[2] == 0 && f->payload[3] == 0 &&
               f->payload[4] == 0;
    case 4:
    case 5:
        return f->length == 0;
    case 6:
        return f->length >= 9 && f->length <= 40;
    case 7:
        return f->length == 24;
    case 8:
        return f->length == 37;
    case 9:
        return f->length == 5;
    default:
        return false;
    }
}
static inline bool UsbControlSequenceForward(uint32_t next, uint32_t previous)
{
    uint32_t delta = next - previous;
    return delta != 0 && delta < 0x80000000u;
}
