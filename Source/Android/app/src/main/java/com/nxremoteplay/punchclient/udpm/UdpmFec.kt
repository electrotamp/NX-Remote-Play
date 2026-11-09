package com.nxremoteplay.punchclient.udpm

/**
 * Kotlin mirror of udpm_fec.c: systematic Cauchy Reed-Solomon over GF(2^8), polynomial 0x11D. Must
 * stay byte-identical with the host (UdpmFecTest checks a vector produced by the C code).
 */
object UdpmFec {
    private val exp = IntArray(512)
    private val log = IntArray(256)

    init {
        var x = 1
        for (i in 0 until 255) {
            exp[i] = x
            log[x] = i
            x = x shl 1
            if (x and 0x100 != 0) x = x xor 0x11D
        }
        for (i in 255 until 512) exp[i] = exp[i - 255]
    }

    fun mul(a: Int, b: Int): Int = if (a == 0 || b == 0) 0 else exp[log[a] + log[b]]

    fun inv(a: Int): Int = exp[255 - log[a]]

    fun coeff(m: Int, p: Int, d: Int): Int = inv(p xor (m + d))

    private fun mulAdd(dst: ByteArray, dOff: Int, src: ByteArray, sOff: Int, c: Int, size: Int) {
        if (c == 0) return
        val lc = log[c]
        for (i in 0 until size) {
            val s = src[sOff + i].toInt() and 0xFF
            if (s != 0) dst[dOff + i] = (dst[dOff + i].toInt() xor exp[log[s] + lc]).toByte()
        }
    }



    /**
     * Rebuilds missing data shards in place. Returns rebuilt count, or -1 with fewer than k shards.
     */
    fun decode(buf: ByteArray, present: BooleanArray, k: Int, m: Int, size: Int): Int {
        val missing = (0 until k).filter { !present[it] }
        if (missing.isEmpty()) return 0
        val rows = (0 until k + m).filter { present[it] }.take(k)
        if (rows.size < k) return -1
        val a =
            Array(k) { r ->
                IntArray(k) { c ->
                    val s = rows[r]
                    if (s < k) (if (s == c) 1 else 0) else coeff(m, s - k, c)
                }
            }
        val inv = Array(k) { r -> IntArray(k) { c -> if (r == c) 1 else 0 } }
        for (c in 0 until k) {
            var piv = c
            while (piv < k && a[piv][c] == 0) piv++
            if (piv == k) return -1
            if (piv != c) {
                val t = a[c]
                a[c] = a[piv]
                a[piv] = t
                val u = inv[c]
                inv[c] = inv[piv]
                inv[piv] = u
            }
            val f = inv(a[c][c])
            for (j in 0 until k) {
                a[c][j] = mul(a[c][j], f)
                inv[c][j] = mul(inv[c][j], f)
            }
            for (r in 0 until k) {
                if (r == c || a[r][c] == 0) continue
                val g = a[r][c]
                for (j in 0 until k) {
                    a[r][j] = a[r][j] xor mul(g, a[c][j])
                    inv[r][j] = inv[r][j] xor mul(g, inv[c][j])
                }
            }
        }
        for (d in missing) {
            val o = d * size
            java.util.Arrays.fill(buf, o, o + size, 0)
            for (r in 0 until k) mulAdd(buf, o, buf, rows[r] * size, inv[d][r], size)
        }
        return missing.size
    }
}
