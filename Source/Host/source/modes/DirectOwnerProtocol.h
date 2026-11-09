#pragma once
#include "BridgeOwnerProtocol.h"
#define DIRECT_OWNER_MAGIC 0x3144584Eu
static inline void OwnerPut32(uint8_t *p, uint32_t n)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(n >> (8 * i));
}
static inline void OwnerPut64(uint8_t *p, uint64_t n)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(n >> (8 * i));
}
static inline void DirectOwnerHeader(uint8_t *bytes, size_t size, const char *id, uint8_t type, uint8_t action,
                                     uint64_t generation, uint64_t correlation)
{
    memset(bytes, 0, size);
    OwnerPut32(bytes, DIRECT_OWNER_MAGIC);
    bytes[4] = 1;
    bytes[5] = type;
    bytes[6] = action;
    memcpy(bytes + 8, id, 5);
    OwnerPut64(bytes + 16, generation);
    OwnerPut64(bytes + 24, correlation);
}
static inline bool DirectOwnerReplyValid(const uint8_t *bytes, size_t size, const char *id, uint8_t type,
                                         uint8_t action, uint64_t correlation)
{
    return size >= 64 && size <= USB_OWNER_MAX + 64 && OwnerU32(bytes) == DIRECT_OWNER_MAGIC && bytes[4] == 1 &&
           bytes[5] == type && bytes[6] == action && bytes[7] <= 2 && memcmp(bytes + 8, id, 5) == 0 && !bytes[13] &&
           !bytes[14] && !bytes[15] && OwnerU64(bytes + 16) > 0 && OwnerU64(bytes + 24) == correlation;
}
