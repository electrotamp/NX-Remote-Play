// Systematic Reed-Solomon erasure code over GF(2^8), polynomial 0x11D (the same
// field Nintendo's GameShare client uses). Parity rows form a Cauchy matrix, so
// any k of the k+m shards rebuild the block.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "udpm_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

void UdpmGfInit(void); // idempotent; builds log/exp tables
uint8_t UdpmGfMul(uint8_t a, uint8_t b);
uint8_t UdpmGfInv(uint8_t a); // a != 0

// Cauchy coefficient for parity row `p` (0..m-1) and data column `d` (0..k-1).
uint8_t UdpmFecCoeff(unsigned m, unsigned p, unsigned d);

// parity[p] = sum_d coeff(p,d) * data[d], each shard `size` bytes.
// k <= UDPM_MAX_DATA_SHARDS, m <= UDPM_MAX_PARITY_SHARDS.
void UdpmFecEncode(const uint8_t* const* data, unsigned k, uint8_t* const* parity, unsigned m, size_t size);

// Rebuilds missing data shards in place.
// shards[0..k+m-1]: buffers of `size` bytes; present[i] says whether shard i arrived.
// Missing data buffers must be writable. Returns the number of data shards rebuilt,
// or -1 when fewer than k shards are present.
int UdpmFecDecode(uint8_t* const* shards, const uint8_t* present, unsigned k, unsigned m, size_t size);

#ifdef __cplusplus
}
#endif
