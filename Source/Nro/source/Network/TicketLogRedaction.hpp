#pragma once
#include <string>

// Redact only the valid 48-bit connection ticket; parsing still uses the original reply.
inline std::string RedactTicketReply(const std::string &reply)
{
    if (reply.rfind("OK ", 0) != 0)
        return reply;
    const size_t separator = reply.find(' ', 3);
    if (separator == std::string::npos || reply.size() - separator - 1 != 12)
        return reply;
    for (size_t i = separator + 1; i < reply.size(); ++i)
    {
        const char c = reply[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return reply;
    }
    return reply.substr(0, separator + 1) + "[redacted-ticket]";
}
