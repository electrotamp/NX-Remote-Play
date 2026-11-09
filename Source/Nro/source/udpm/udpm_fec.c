#include "udpm_fec.h"
#include <string.h>

static uint8_t gfExp[512];
static uint8_t gfLog[256];
static int gfReady;

void UdpmGfInit(void)
{
    if (gfReady)
        return;
    unsigned x = 1;
    for (unsigned i = 0; i < 255; i++)
    {
        gfExp[i] = (uint8_t)x;
        gfLog[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100)
            x ^= 0x11D;
    }
    for (unsigned i = 255; i < 512; i++)
        gfExp[i] = gfExp[i - 255];
    gfLog[0] = 0;
    gfReady = 1;
}

uint8_t UdpmGfMul(uint8_t a, uint8_t b)
{
    if (!a || !b)
        return 0;
    return gfExp[gfLog[a] + gfLog[b]];
}

uint8_t UdpmGfInv(uint8_t a)
{
    return gfExp[255 - gfLog[a]];
}

// Cauchy element 1/(x_p ^ y_d) with x_p = p and y_d = m + d. All x and y values
// are distinct while k + m <= 256, so every square submatrix is invertible.
uint8_t UdpmFecCoeff(unsigned m, unsigned p, unsigned d)
{
    return UdpmGfInv((uint8_t)(p ^ (m + d)));
}

// dst ^= c * src
static void mulAdd(uint8_t *dst, const uint8_t *src, uint8_t c, size_t size)
{
    if (!c)
        return;
    if (c == 1)
    {
        for (size_t i = 0; i < size; i++)
            dst[i] ^= src[i];
        return;
    }
    const unsigned lc = gfLog[c];
    for (size_t i = 0; i < size; i++)
    {
        const uint8_t s = src[i];
        if (s)
            dst[i] ^= gfExp[gfLog[s] + lc];
    }
}

void UdpmFecEncode(const uint8_t *const *data, unsigned k, uint8_t *const *parity, unsigned m, size_t size)
{
    UdpmGfInit();
    for (unsigned p = 0; p < m; p++)
    {
        memset(parity[p], 0, size);
        for (unsigned d = 0; d < k; d++)
            mulAdd(parity[p], data[d], UdpmFecCoeff(m, p, d), size);
    }
}

int UdpmFecDecode(uint8_t *const *shards, const uint8_t *present, unsigned k, unsigned m, size_t size)
{
    UdpmGfInit();
    unsigned missing[UDPM_MAX_DATA_SHARDS], nMissing = 0;
    for (unsigned d = 0; d < k; d++)
        if (!present[d])
            missing[nMissing++] = d;
    if (!nMissing)
        return 0;
    unsigned rows[UDPM_MAX_DATA_SHARDS], nRows = 0; // shard index used for each matrix row
    for (unsigned i = 0; i < k + m && nRows < k; i++)
        if (present[i])
            rows[nRows++] = i;
    if (nRows < k)
        return -1;

    // A (k x k): row r is the generator row of shard rows[r]. Invert via Gauss-Jordan.
    static uint8_t a[UDPM_MAX_DATA_SHARDS][UDPM_MAX_DATA_SHARDS];
    static uint8_t inv[UDPM_MAX_DATA_SHARDS][UDPM_MAX_DATA_SHARDS];
    for (unsigned r = 0; r < k; r++)
    {
        for (unsigned c = 0; c < k; c++)
        {
            const unsigned s = rows[r];
            a[r][c] = s < k ? (uint8_t)(s == c) : UdpmFecCoeff(m, s - k, c);
            inv[r][c] = (uint8_t)(r == c);
        }
    }
    for (unsigned c = 0; c < k; c++)
    {
        unsigned piv = c;
        while (piv < k && !a[piv][c])
            piv++;
        if (piv == k)
            return -1; // cannot happen for a Cauchy code
        if (piv != c)
        {
            for (unsigned j = 0; j < k; j++)
            {
                uint8_t t = a[c][j];
                a[c][j] = a[piv][j];
                a[piv][j] = t;
                t = inv[c][j];
                inv[c][j] = inv[piv][j];
                inv[piv][j] = t;
            }
        }
        const uint8_t f = UdpmGfInv(a[c][c]);
        for (unsigned j = 0; j < k; j++)
        {
            a[c][j] = UdpmGfMul(a[c][j], f);
            inv[c][j] = UdpmGfMul(inv[c][j], f);
        }
        for (unsigned r = 0; r < k; r++)
        {
            if (r == c || !a[r][c])
                continue;
            const uint8_t g = a[r][c];
            for (unsigned j = 0; j < k; j++)
            {
                a[r][j] ^= UdpmGfMul(g, a[c][j]);
                inv[r][j] ^= UdpmGfMul(g, inv[c][j]);
            }
        }
    }
    // data[d] = sum_r inv[d][r] * shard(rows[r]); only rows for missing data are needed.
    for (unsigned i = 0; i < nMissing; i++)
    {
        const unsigned d = missing[i];
        uint8_t *out = shards[d];
        memset(out, 0, size);
        for (unsigned r = 0; r < k; r++)
        {
            if (rows[r] == d)
                continue; // never true: d is missing
            mulAdd(out, shards[rows[r]], inv[d][r], size);
        }
    }
    return (int)nMissing;
}
