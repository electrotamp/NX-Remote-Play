#include "UpdateCheck.hpp"

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#include "DnsClient.hpp"
#include <arpa/inet.h>
#include <curl/curl.h>

namespace Network::UpdateCheck
{
namespace
{
std::atomic<State> g_state{State::NotChecked};
std::atomic<bool> g_stop{false};
std::mutex g_mutex;
std::string g_latest;
std::thread g_thread;
std::string g_statusText;

bool Parse(std::string s, long long core[3], std::vector<std::string> &pre)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.erase(0, 1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
        s.pop_back();
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V'))
        s.erase(0, 1);
    const size_t plus = s.find('+');
    if (plus != std::string::npos)
        s.resize(plus);
    pre.clear();
    const size_t dash = s.find('-');
    if (dash != std::string::npos)
    {
        std::string rest = s.substr(dash + 1);
        s.resize(dash);
        size_t start = 0;
        for (;;)
        {
            const size_t dot = rest.find('.', start);
            pre.push_back(rest.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
            if (pre.back().empty())
                return false;
            if (dot == std::string::npos)
                break;
            start = dot + 1;
        }
    }
    int part = 0;
    size_t start = 0;
    for (;;)
    {
        const size_t dot = s.find('.', start);
        const std::string digits = s.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (part >= 3 || digits.empty() || digits.size() > 18 || digits.find_first_not_of("0123456789") != std::string::npos)
            return false;
        core[part++] = std::atoll(digits.c_str());
        if (dot == std::string::npos)
            break;
        start = dot + 1;
    }
    return part == 3;
}

bool IsNumber(const std::string &s)
{
    return !s.empty() && s.size() <= 18 && s.find_first_not_of("0123456789") == std::string::npos;
}

int Sign(long long v)
{
    return v > 0 ? 1 : v < 0 ? -1 : 0;
}

constexpr const char *ApiHost = "api.github.com";
constexpr const char *ApiUrl = "https://api.github.com/repos/ElectroTamp/NX-Remote-Play/releases?per_page=10";
constexpr size_t MaxBody = 512 * 1024;

size_t Collect(char *data, size_t size, size_t count, void *user)
{
    auto *body = static_cast<std::string *>(user);
    const size_t bytes = size * count;
    if (body->size() + bytes > MaxBody)
        return 0;
    body->append(data, bytes);
    return bytes;
}

int Abort(void *, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    return g_stop ? 1 : 0;
}

// The NRO resolves names with its own DNS client; curl is handed the address so it never calls the system resolver.
bool Fetch(std::string &body)
{
    uint32_t ip = 0;
    if (!Network::Dns::ResolveBlocking(ApiHost, ip, 3000))
        return false;
    char address[INET_ADDRSTRLEN] = {};
    in_addr in{};
    in.s_addr = ip;
    inet_ntop(AF_INET, &in, address, sizeof(address));
    const std::string resolve = std::string(ApiHost) + ":443:" + address;

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        return false;
    bool ok = false;
    if (CURL *curl = curl_easy_init())
    {
        curl_slist *resolveList = curl_slist_append(nullptr, resolve.c_str());
        curl_slist *headers = curl_slist_append(nullptr, "Accept: application/vnd.github+json");
        curl_easy_setopt(curl, CURLOPT_URL, ApiUrl);
        curl_easy_setopt(curl, CURLOPT_RESOLVE, resolveList);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "NX-Remote-Play-Switch/" NXRP_APP_VERSION);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 12L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, Abort);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Collect);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        long status = 0;
        if (curl_easy_perform(curl) == CURLE_OK && curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status) == CURLE_OK)
            ok = status == 200;
        curl_slist_free_all(headers);
        curl_slist_free_all(resolveList);
        curl_easy_cleanup(curl);
    }
    curl_global_cleanup();
    return ok;
}

void Run()
{
    std::string body;
    State result = State::Failed;
    if (Fetch(body))
    {
        const std::string newest = NewestTag(body);
        result = State::UpToDate;
        if (!newest.empty() && Compare(newest, NXRP_APP_VERSION) > 0)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_latest = newest[0] == 'v' || newest[0] == 'V' ? newest.substr(1) : newest;
            result = State::Available;
        }
    }
    g_state = result;
}
} // namespace

int Compare(const std::string &a, const std::string &b)
{
    long long coreA[3], coreB[3];
    std::vector<std::string> preA, preB;
    if (!Parse(a, coreA, preA) || !Parse(b, coreB, preB))
        return -2;
    for (int i = 0; i < 3; ++i)
        if (coreA[i] != coreB[i])
            return Sign(coreA[i] - coreB[i]);
    if (preA.empty() || preB.empty())
        return preA.size() == preB.size() ? 0 : preA.empty() ? 1 : -1;
    for (size_t i = 0; i < preA.size() && i < preB.size(); ++i)
    {
        const bool numA = IsNumber(preA[i]), numB = IsNumber(preB[i]);
        int order;
        if (numA && numB)
            order = Sign(std::atoll(preA[i].c_str()) - std::atoll(preB[i].c_str()));
        else if (numA || numB)
            order = numA ? -1 : 1;
        else
            order = Sign(preA[i].compare(preB[i]));
        if (order != 0)
            return order;
    }
    return Sign(static_cast<long long>(preA.size()) - static_cast<long long>(preB.size()));
}


// A release object lists "tag_name" before "draft"; each tag is paired with the next "draft" value before the next tag.
std::string NewestTag(const std::string &json)
{
    std::string newest;
    size_t at = 0;
    for (;;)
    {
        at = json.find("\"tag_name\"", at);
        if (at == std::string::npos)
            break;
        const size_t colon = json.find(':', at + 10);
        const size_t open = colon == std::string::npos ? colon : json.find('"', colon + 1);
        const size_t close = open == std::string::npos ? open : json.find('"', open + 1);
        if (close == std::string::npos)
            break;
        const std::string tag = json.substr(open + 1, close - open - 1);
        const size_t draft = json.find("\"draft\"", close);
        const size_t nextTag = json.find("\"tag_name\"", close);
        bool isDraft = false;
        if (draft != std::string::npos && (nextTag == std::string::npos || draft < nextTag))
        {
            const size_t value = json.find_first_not_of(" \t\r\n:", draft + 7);
            isDraft = value != std::string::npos && json.compare(value, 4, "true") == 0;
        }
        if (!isDraft && Compare(tag, "0.0.0") != -2 && (newest.empty() || Compare(tag, newest) > 0))
            newest = tag;
        at = close;
    }
    return newest;
}

void Start()
{
    if (g_state == State::Checking)
        return;
    if (g_thread.joinable())
        g_thread.join();
    g_state = State::Checking;
    g_thread = std::thread(Run);
}

void Join()
{
    g_stop = true;
    if (g_thread.joinable())
        g_thread.join();
}

State Current()
{
    return g_state;
}

std::string Latest()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_latest;
}

const char *StatusText()
{
    switch (g_state.load())
    {
    case State::Checking:
        return "Checking...";
    case State::UpToDate:
        return "Up to date";
    case State::Available:
        g_statusText = "Update available: " + Latest();
        return g_statusText.c_str();
    case State::Failed:
        return "Could not check";
    default:
        return "Not checked";
    }
}
} // namespace Network::UpdateCheck
