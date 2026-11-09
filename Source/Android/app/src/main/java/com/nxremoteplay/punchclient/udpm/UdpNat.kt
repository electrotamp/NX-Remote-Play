package com.nxremoteplay.punchclient.udpm

import java.net.InetAddress
import java.net.InetSocketAddress

/**  capability-authenticated endpoint discovery. A capability comes from the TCP handshake. */
object UdpNat {
    const val MAGIC = 0x3150554e

    fun packet(role: Int, session: Int, key: ByteArray): ByteArray {
        require(key.size == 16)
        return ByteArray(28).apply {
            this[0] = 78
            this[1] = 85
            this[2] = 80
            this[3] = 49
            this[4] = role.toByte()
            for (i in 0..3) this[8 + i] = (session ushr (8 * i)).toByte()
            key.copyInto(this, 12)
        }
    }

    fun matches(b: ByteArray, size: Int, session: Int, key: ByteArray): Boolean {
        if (size !in listOf(28, 36) || size > b.size || key.size != 16) return false
        val expected = packet(b[4].toInt(), session, key)
        var diff = 0
        for (i in 0 until 28) diff = diff or (expected[i].toInt() xor b[i].toInt())
        return diff == 0
    }

    fun peer(b: ByteArray, size: Int, session: Int, key: ByteArray): InetSocketAddress? {
        if (size != 36 ||
            !matches(b, size, session, key) ||
            b[4].toInt() != 3 ||
            b[34].toInt() != 0 ||
            b[35].toInt() != 0)
            return null
        val address = InetAddress.getByAddress(b.copyOfRange(28, 32))
        val port = ((b[32].toInt() and 255) shl 8) or (b[33].toInt() and 255)
        if (port == 0 || address.isAnyLocalAddress || address.isMulticastAddress) return null
        return InetSocketAddress(address, port)
    }
}
