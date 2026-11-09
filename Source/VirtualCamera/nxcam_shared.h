// Shared between NXRemotePlayCam.dll (the DirectShow camera, the reader) and the desktop app (the producer).
// The producer creates the mapping NXCAM_MAP_NAME and writes the newest decoded picture into a slot; the camera copies
// it out. Layout (little endian, offsets in bytes): header 64 bytes inside a 4096-byte header block, then NXCAM_SLOTS
// frame slots.
#pragma once
#include <stdint.h>

#define NXCAM_MAP_NAME   L"Local\\NXRemotePlayCamera"
#define NXCAM_MAGIC      0x4D43584Eu   // "NXCM"
#define NXCAM_VERSION    1u
#define NXCAM_W          1280u
#define NXCAM_H          720u
#define NXCAM_STRIDE     (NXCAM_W * 4u)   // BGRA, top-down
#define NXCAM_SLOTS      3u
#define NXCAM_SLOT_BYTES (NXCAM_STRIDE * NXCAM_H)
#define NXCAM_HEADER_BLOCK 4096u
#define NXCAM_MAP_BYTES  (NXCAM_HEADER_BLOCK + NXCAM_SLOTS * NXCAM_SLOT_BYTES)

#pragma pack(push, 8)
struct NxCamHeader
{
    uint32_t magic;       //  0
    uint32_t version;     //  4
    uint32_t width;       //  8
    uint32_t height;      // 12
    uint32_t stride;      // 16
    uint32_t slots;       // 20
    uint32_t slotBytes;   // 24
    uint32_t headerBytes; // 28 (= NXCAM_HEADER_BLOCK)
    volatile uint32_t
        readers; // 32 camera streams currently open (informational; can stay too high if a program crashes)
    volatile uint32_t latestSlot;   // 36 slot holding the newest complete picture
    volatile uint64_t readerTickMs; // 40 GetTickCount64() of the camera, refreshed with every picture it delivers (the
                                    // app converts only while this is fresh)
    volatile uint64_t frameSeq;     // 48 increases with every picture
    volatile uint64_t producerTickMs; // 56 GetTickCount64() of the producer, refreshed at least every 500 ms
};
#pragma pack(pop)
static_assert(sizeof(NxCamHeader) == 64, "NxCamHeader layout");
