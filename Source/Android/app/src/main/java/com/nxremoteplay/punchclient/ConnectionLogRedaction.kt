package com.nxremoteplay.punchclient

// The connection ticket grants access and must not enter app logs or logcat.
internal fun redactConnectionTicketReply(reply: String?): String {
    if (reply == null) return "null"
    return Regex("^(OK\\s+\\S+\\s+)[0-9a-fA-F]{12}(?=\\s|$)")
        .replace(reply, "$1[redacted-ticket]")
}
