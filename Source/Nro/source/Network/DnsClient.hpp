#pragma once
#include <cstdint>
#include <string>

// Name resolution for the NRO (everything must resolve domain names).
// A minimal DNS A-record client over UDP: no helper threads and no system resolver call. It asks the console's own
// DNS servers first (when nifm can report them), then 1.1.1.1 and 8.8.8.8. IPv4 literals are returned as they are.
// Answers are cached for 5 minutes; a failed lookup is not retried more often than every 10 seconds.
namespace Network::Dns
{
// Never blocks. The first call for a name sends the query; later calls poll for the answer.
// True (and ip, network byte order) once the name is an IPv4 literal or has been resolved.
bool Resolve(const std::string &host, uint32_t &ip);

// For worker threads: waits up to timeoutMs for the answer.
bool ResolveBlocking(const std::string &host, uint32_t &ip, int timeoutMs = 3000);
} // namespace Network::Dns
