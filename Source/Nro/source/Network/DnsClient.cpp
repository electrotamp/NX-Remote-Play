#include "DnsClient.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>

#include <switch.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

namespace Network::Dns
{
namespace
{
using Clock = std::chrono::steady_clock;

std::mutex cacheMutex;
std::string cachedName;
uint32_t cachedIp = INADDR_NONE;
Clock::time_point cachedAt;

std::string failedName;
Clock::time_point failedAt;

// ---- query/answer packets
size_t BuildQuery(const std::string &host, uint16_t txid, uint8_t *q, size_t cap)
{
    size_t n = 0;
    q[n++] = (uint8_t)(txid >> 8);
    q[n++] = (uint8_t)txid;
    q[n++] = 0x01;
    q[n++] = 0x00; // recursion desired
    q[n++] = 0;
    q[n++] = 1; // one question
    for (int i = 0; i < 6; i++)
        q[n++] = 0; // no answer/authority/additional records
    size_t pos = 0;
    while (pos < host.size())
    {
        size_t dot = host.find('.', pos);
        if (dot == std::string::npos)
            dot = host.size();
        const size_t len = dot - pos;
        if (len == 0 || len > 63 || n + len + 6 > cap)
            return 0;
        q[n++] = (uint8_t)len;
        memcpy(q + n, host.data() + pos, len);
        n += len;
        pos = dot + 1;
    }
    q[n++] = 0;
    q[n++] = 0;
    q[n++] = 1; // type A
    q[n++] = 0;
    q[n++] = 1; // class IN
    return n;
}

bool ParseAnswer(const uint8_t *r, size_t n, uint16_t txid, uint32_t &ip)
{
    if (n < 12 || r[0] != (uint8_t)(txid >> 8) || r[1] != (uint8_t)txid || (r[3] & 0x0F) != 0)
        return false;
    int an = (r[6] << 8) | r[7];
    size_t pos = 12;
    for (int qd = (r[4] << 8) | r[5]; qd > 0 && pos < n; qd--)
    {
        while (pos < n && r[pos] != 0)
            pos += (r[pos] & 0xC0) == 0xC0 ? 2 : (size_t)r[pos] + 1;
        pos += 5;
    }
    for (; an > 0 && pos + 12 <= n; an--)
    {
        while (pos < n && r[pos] != 0 && (r[pos] & 0xC0) != 0xC0)
            pos += (size_t)r[pos] + 1;
        pos += (pos < n && (r[pos] & 0xC0) == 0xC0) ? 2 : 1;
        if (pos + 10 > n)
            return false;
        const int type = (r[pos] << 8) | r[pos + 1];
        const int rdlen = (r[pos + 8] << 8) | r[pos + 9];
        pos += 10;
        if (type == 1 && rdlen == 4 && pos + 4 <= n)
        {
            memcpy(&ip, r + pos, 4);
            return true;
        }
        pos += (size_t)rdlen;
    }
    return false;
}

std::vector<uint32_t> DnsServers()
{
    std::vector<uint32_t> v;
    static bool nifmTried = false, nifmOk = false;
    if (!nifmTried)
    {
        nifmTried = true;
        nifmOk = R_SUCCEEDED(nifmInitialize(NifmServiceType_User));
    }
    if (nifmOk)
    {
        u32 a = 0, m = 0, g = 0, d1 = 0, d2 = 0;
        if (R_SUCCEEDED(nifmGetCurrentIpConfigInfo(&a, &m, &g, &d1, &d2)))
        {
            if (d1)
                v.push_back(d1);
            if (d2)
                v.push_back(d2);
        }
    }
    v.push_back(inet_addr("1.1.1.1"));
    v.push_back(inet_addr("8.8.8.8"));
    return v;
}

int OpenNonBlocking()
{
    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return -1;
    const int fl = fcntl(s, F_GETFL, 0);
    if (fl >= 0)
        fcntl(s, F_SETFL, fl | O_NONBLOCK);
    return s;
}

bool SendQuery(int s, uint32_t server, const uint8_t *q, size_t n)
{
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    to.sin_addr.s_addr = server;
    return sendto(s, q, n, 0, (sockaddr *)&to, sizeof(to)) == (ssize_t)n;
}

bool CachedLookup(const std::string &host, uint32_t &ip)
{
    std::lock_guard<std::mutex> lock(cacheMutex);
    if (cachedIp != INADDR_NONE && cachedName == host && Clock::now() - cachedAt < std::chrono::minutes(5))
    {
        ip = cachedIp;
        return true;
    }
    return false;
}

void Remember(const std::string &host, uint32_t ip)
{
    std::lock_guard<std::mutex> lock(cacheMutex);
    cachedName = host;
    cachedIp = ip;
    cachedAt = Clock::now();
}

// ---- the single non-blocking query used by Resolve() (UI thread only)
struct Pending
{
    bool active = false;
    std::string name;
    int sock = -1;
    uint16_t txid = 0;
    size_t serverIndex = 0;
    std::vector<uint32_t> servers;
    uint8_t query[300];
    size_t queryLen = 0;
    Clock::time_point sentAt;
} pending;

void ClosePending()
{
    if (pending.sock >= 0)
        close(pending.sock);
    pending = Pending();
}

void Fail(const std::string &host)
{
    failedName = host;
    failedAt = Clock::now();
    ClosePending();
}
} // namespace

bool Resolve(const std::string &host, uint32_t &ip)
{
    if (host.empty())
        return false;
    const in_addr_t literal = inet_addr(host.c_str());
    if (literal != INADDR_NONE)
    {
        ip = literal;
        return true;
    }
    if (CachedLookup(host, ip))
        return true;

    if (!pending.active || pending.name != host)
    {
        if (pending.active)
            ClosePending();
        if (failedName == host && Clock::now() - failedAt < std::chrono::seconds(10))
            return false;
        pending.sock = OpenNonBlocking();
        if (pending.sock < 0)
        {
            Fail(host);
            return false;
        }
        pending.active = true;
        pending.name = host;
        pending.txid = (uint16_t)(Clock::now().time_since_epoch().count() & 0xFFFF);
        pending.servers = DnsServers();
        pending.queryLen = BuildQuery(host, pending.txid, pending.query, sizeof(pending.query));
        if (pending.queryLen == 0)
        {
            Fail(host);
            return false;
        }
        pending.serverIndex = 0;
        SendQuery(pending.sock, pending.servers[0], pending.query, pending.queryLen);
        pending.sentAt = Clock::now();
        return false;
    }

    uint8_t resp[512];
    const ssize_t n = recvfrom(pending.sock, resp, sizeof(resp), 0, nullptr, nullptr);
    if (n > 0)
    {
        uint32_t found = INADDR_NONE;
        if (ParseAnswer(resp, (size_t)n, pending.txid, found))
        {
            Remember(host, found);
            ClosePending();
            ip = found;
            return true;
        }
    }
    if (Clock::now() - pending.sentAt >= std::chrono::milliseconds(1000))
    {
        if (++pending.serverIndex >= pending.servers.size())
        {
            Fail(host);
            return false;
        }
        SendQuery(pending.sock, pending.servers[pending.serverIndex], pending.query, pending.queryLen);
        pending.sentAt = Clock::now();
    }
    return false;
}

bool ResolveBlocking(const std::string &host, uint32_t &ip, int timeoutMs)
{
    if (host.empty())
        return false;
    const in_addr_t literal = inet_addr(host.c_str());
    if (literal != INADDR_NONE)
    {
        ip = literal;
        return true;
    }
    if (CachedLookup(host, ip))
        return true;

    const int s = OpenNonBlocking();
    if (s < 0)
        return false;
    const uint16_t txid = (uint16_t)(Clock::now().time_since_epoch().count() & 0xFFFF);
    uint8_t q[300];
    const size_t qn = BuildQuery(host, txid, q, sizeof(q));
    bool ok = false;
    if (qn)
    {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        for (uint32_t server : DnsServers())
        {
            if (Clock::now() >= deadline)
                break;
            SendQuery(s, server, q, qn);
            const auto until = std::min(deadline, Clock::now() + std::chrono::milliseconds(1000));
            while (!ok && Clock::now() < until)
            {
                uint8_t resp[512];
                const ssize_t n = recvfrom(s, resp, sizeof(resp), 0, nullptr, nullptr);
                if (n > 0 && ParseAnswer(resp, (size_t)n, txid, ip))
                {
                    ok = true;
                    break;
                }
                svcSleepThread(20ULL * 1000 * 1000);
            }
            if (ok)
                break;
        }
    }
    close(s);
    if (ok)
        Remember(host, ip);
    return ok;
}

} // namespace Network::Dns
