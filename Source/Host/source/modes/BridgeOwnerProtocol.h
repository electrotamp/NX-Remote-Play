#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#define USB_OWNER_MAGIC 0x3152584Eu
#define USB_OWNER_MAX 640u
#define USB_OWNER_KEY_MAGIC 0x314B584Eu
typedef struct
{
    const uint8_t *id, *owner, *endpoint;
    uint16_t ownerLength, endpointLength;
    uint8_t action;
    uint64_t generation;
} UsbOwnerRequest;
static inline uint16_t OwnerU16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline uint32_t OwnerU32(const uint8_t *p)
{
    return (uint32_t)OwnerU16(p) | ((uint32_t)OwnerU16(p + 2) << 16);
}
static inline uint64_t OwnerU64(const uint8_t *p)
{
    return (uint64_t)OwnerU32(p) | ((uint64_t)OwnerU32(p + 4) << 32);
}
static inline bool OwnerField(const uint8_t *bytes, size_t size, size_t *at, const uint8_t **value, uint16_t *length,
                              uint16_t maximum)
{
    if (*at > size || size - *at < 2)
        return false;
    *length = OwnerU16(bytes + *at);
    *at += 2;
    if (*length == 0 || *length > maximum || size - *at < *length)
        return false;
    *value = bytes + *at;
    *at += *length;
    for (uint16_t i = 0; i < *length; i++)
        if ((*value)[i] < 33 || (*value)[i] > 126)
            return false;
    return true;
}
// Exactly the rendezvous v1 HMAC canonicalization. USB may only sign bridge operations;
// this is not a general-purpose HMAC oracle or a way to authorize another console.
static inline bool OwnerParse(const uint8_t *bytes, size_t size, const char *id, uint8_t kind, UsbOwnerRequest *request)
{
    const size_t domainSize = sizeof("NXRP-OWNER-1") - 1;
    if (size > USB_OWNER_MAX || size < domainSize || memcmp(bytes, "NXRP-OWNER-1", domainSize) != 0)
        return false;
    size_t at = domainSize;
    uint16_t length;
    const uint8_t *nonce;
    if (!OwnerField(bytes, size, &at, &request->id, &length, 5) || length != 5 || strlen(id) != 5 ||
        memcmp(request->id, id, 5) != 0)
        return false;
    if (size - at < 2)
        return false;
    request->action = bytes[at++];
    if (request->action > 2 || bytes[at++] != kind)
        return false;
    if (!OwnerField(bytes, size, &at, &request->owner, &request->ownerLength, 128) ||
        !OwnerField(bytes, size, &at, &request->endpoint, &request->endpointLength, 256) || size - at < 8)
        return false;
    request->generation = OwnerU64(bytes + at);
    at += 8;
    if (request->generation == 0 || !OwnerField(bytes, size, &at, &nonce, &length, 64) || length != 64 ||
        size - at != 8)
        return false;
    for (size_t i = 0; i < 64; i++)
        if (!((nonce[i] >= '0' && nonce[i] <= '9') || (nonce[i] >= 'A' && nonce[i] <= 'F')))
            return false;
    return OwnerU64(bytes + at) > 0 && OwnerU64(bytes + at) <= INT64_MAX;
}
static inline bool UsbOwnerParse(const uint8_t *bytes, size_t size, const char *id, UsbOwnerRequest *request)
{
    return OwnerParse(bytes, size, id, 1, request);
}
typedef struct
{
    uint32_t token;
    uint16_t total, have;
    uint64_t started;
    uint8_t bytes[USB_OWNER_MAX];
} UsbOwnerAssembly;
// Returns 1 complete, 0 partial, -1 invalid. Deadline is fixed from the first fragment.
static inline int UsbOwnerAppend(UsbOwnerAssembly *assembly, const uint8_t *payload, size_t length, uint64_t now,
                                 uint64_t timeout)
{
    if (length < 9 || length > 40)
    {
        assembly->have = 0;
        return -1;
    }
    uint32_t token = OwnerU32(payload);
    uint16_t total = OwnerU16(payload + 4), offset = OwnerU16(payload + 6);
    if (!token || !total || total > USB_OWNER_MAX || offset > total || length - 8 > (size_t)(total - offset))
    {
        assembly->have = 0;
        return -1;
    }
    if (offset == 0)
    {
        assembly->token = token;
        assembly->total = total;
        assembly->have = 0;
        assembly->started = now;
    }
    if (assembly->token != token || assembly->total != total || assembly->have != offset ||
        now - assembly->started >= timeout)
    {
        assembly->have = 0;
        return -1;
    }
    memcpy(assembly->bytes + offset, payload + 8, length - 8);
    assembly->have += length - 8;
    return assembly->have == total ? 1 : 0;
}
