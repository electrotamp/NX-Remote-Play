#include "applet_mitm_log.hpp"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <switch/services/set.h>
#include <switch/services/spl.h>

namespace ams::mitm::applet
{

constinit Stats g_stats = {};

namespace
{

const char *LogPath = "sdmc:/config/nxremoteplay/host_log.txt";
const char *LastPath = "sdmc:/config/nxremoteplay/host_last.txt";
/* a release install logs into its config folder, and stops at
 * 4 MB - a test run's log is read the same day; a user's can run
 * for weeks of boots without anyone looking */
constexpr s64 ReleaseLogCap = 4 * 1024 * 1024;
constinit os::SdkMutex g_lock;
constinit bool g_ready = false;
/* held in memory while a teardown races an applet - each
 * flushed line costs the SD ~15 ms, and 20 of them lost the race */
constinit bool g_defer = false;
constinit char g_defer_buf[8192];
constinit size_t g_defer_len = 0;
// Producers only copy into these bounded buffers. The writer never holds g_lock during SD I/O.
constinit bool g_async = false;
constinit os::SdkMutex g_write_lock;
constinit os::ThreadType g_writer_thread;
alignas(os::MemoryPageSize) constinit u8 g_writer_stack[16 * 1024];
constinit char g_pending_log[32 * 1024], g_pending_support[8 * 1024], g_pending_mark[128];
constinit size_t g_pending_log_len, g_pending_support_len, g_pending_mark_len;
constinit u64 g_dropped_log, g_dropped_support;
// Only the writer (or an explicit fatal flush) uses this batch, under g_write_lock.
constinit char g_batch_log[sizeof(g_pending_log)], g_batch_support[sizeof(g_pending_support)], g_batch_mark[128];

void QueueLine(char *buffer, size_t capacity, size_t *used, u64 *dropped, const char *data, size_t size)
{
    if (size > capacity - *used)
    {
        ++*dropped;
        return;
    }
    std::memcpy(buffer + *used, data, size);
    *used += size;
}

void AppendToLog(const char *data, size_t n)
{
    fs::FileHandle f;
    if (R_FAILED(fs::OpenFile(std::addressof(f), LogPath, fs::OpenMode_Write | fs::OpenMode_AllowAppend)))
    {
        return;
    }
    s64 size = 0;
    static_cast<void>(fs::GetFileSize(std::addressof(size), f));
    if (size > ReleaseLogCap)
    {
        fs::CloseFile(f);
        return;
    }
    static_cast<void>(fs::WriteFile(f, size, data, n, fs::WriteOption::Flush));
    fs::CloseFile(f);
}

void WriteWhole(const char *path, const char *data, size_t len)
{
    fs::DeleteFile(path);
    if (R_FAILED(fs::CreateFile(path, static_cast<s64>(len))))
    {
        return;
    }
    fs::FileHandle f;
    if (R_FAILED(fs::OpenFile(std::addressof(f), path, fs::OpenMode_Write)))
    {
        return;
    }
    static_cast<void>(fs::WriteFile(f, 0, data, len, fs::WriteOption::Flush));
    fs::CloseFile(f);
}

/* ---- support log  -------------------------------------------------------------------------------
 * sdmc:/config/nxremoteplay/host_support.txt: a short, readable file a user can send for troubleshooting. A header with
 * the console, firmware, Atmosphere, files present and memory, then only the lines that matter (viewer, source, memory,
 * errors, per-stream summaries). No passwords, Session IDs, owner keys or IP addresses are written here. The full
 * developer log (log.txt) is unchanged. 256 KB per file; the previous boot is kept as support.prev.txt. */
constexpr s64 SupportCap = 256 * 1024;
const char *SupPath = "sdmc:/config/nxremoteplay/host_support.txt";
const char *SupPrevPath = "sdmc:/config/nxremoteplay/host_support.prev.txt";
constinit bool g_sup_ready = false;
constinit char g_sup_head[2560];
constinit size_t g_sup_head_len = 0;
constinit char g_sup_defer[4096]; /* lines held while LogDefer is on */
constinit size_t g_sup_defer_len = 0;

void SupAppendRaw(const char *data, size_t n)
{
    fs::FileHandle f;
    if (R_FAILED(fs::OpenFile(std::addressof(f), SupPath, fs::OpenMode_Write | fs::OpenMode_AllowAppend)))
    {
        return;
    }
    s64 size = 0;
    static_cast<void>(fs::GetFileSize(std::addressof(size), f));
    if (size + static_cast<s64>(n) > SupportCap)
    {
        /* rotate: this file becomes the previous one, the new one starts with the header again */
        fs::CloseFile(f);
        static_cast<void>(fs::DeleteFile(SupPrevPath));
        static_cast<void>(fs::RenameFile(SupPath, SupPrevPath));
        static_cast<void>(fs::CreateFile(SupPath, 0));
        if (R_FAILED(fs::OpenFile(std::addressof(f), SupPath, fs::OpenMode_Write | fs::OpenMode_AllowAppend)))
        {
            return;
        }
        size = 0;
        if (g_sup_head_len != 0)
        {
            static_cast<void>(fs::WriteFile(f, size, g_sup_head, g_sup_head_len, fs::WriteOption::Flush));
            size += static_cast<s64>(g_sup_head_len);
        }
    }
    static_cast<void>(fs::WriteFile(f, size, data, n, fs::WriteOption::Flush));
    fs::CloseFile(f);
}

/* "[hh:mm:ss] text" (time since the console booted), no leading spaces; g_lock held by the caller */
void SupEmitLocked(const char *msg)
{
    while (*msg == ' ')
    {
        ++msg;
    }
    const u64 s = armTicksToNs(armGetSystemTick()) / UINT64_C(1000000000);
    char line[420];
    int n = std::snprintf(line, sizeof(line) - 2, "[%02u:%02u:%02u] %s", static_cast<unsigned>(s / 3600),
                          static_cast<unsigned>((s / 60) % 60), static_cast<unsigned>(s % 60), msg);
    if (n < 0)
    {
        return;
    }
    if (static_cast<size_t>(n) > sizeof(line) - 3)
    {
        n = static_cast<int>(sizeof(line) - 3);
    }
    line[n++] = '\n';
    if (g_defer)
    {
        if (g_sup_defer_len + static_cast<size_t>(n) <= sizeof(g_sup_defer))
        {
            std::memcpy(g_sup_defer + g_sup_defer_len, line, static_cast<size_t>(n));
            g_sup_defer_len += static_cast<size_t>(n);
        }
        return;
    }
    if (g_async)
        QueueLine(g_pending_support, sizeof(g_pending_support), &g_pending_support_len, &g_dropped_support,
                  line, static_cast<size_t>(n));
    else
        SupAppendRaw(line, static_cast<size_t>(n));
}

/* which developer-log lines also go to the support file */
bool SupWorthy(const char *m)
{
    static const char *const keys[] = {
        "FATAL",
        "FAILED",
        "donor:",
        "memory yield",
        "nvstream: stopped",
        " frames in ",
        "layers: an overlay",
        "layers: failed",
        "layers: armed",
        "NXRP session",
        "NXRP source",
        "video target",
        "viewer is reading",
        "no donor",
        "not streaming",
        "refused",
        "no heap",
        "wedged",
        "engines:",
        "VIC heap from the donor",
    };
    for (const char *k : keys)
    {
        if (std::strstr(m, k) != nullptr)
        {
            return true;
        }
    }
    return false;
}

} // namespace

void LogFlush()
{
    std::scoped_lock writer(g_write_lock);
    size_t log_size, support_size, mark_size;
    u64 dropped_log, dropped_support;
    {
        std::scoped_lock lk(g_lock);
        log_size = g_pending_log_len;
        support_size = g_pending_support_len;
        mark_size = g_pending_mark_len;
        std::memcpy(g_batch_log, g_pending_log, log_size);
        std::memcpy(g_batch_support, g_pending_support, support_size);
        std::memcpy(g_batch_mark, g_pending_mark, mark_size);
        dropped_log = g_dropped_log;
        dropped_support = g_dropped_support;
        g_pending_log_len = g_pending_support_len = g_pending_mark_len = 0;
        g_dropped_log = g_dropped_support = 0;
    }
    if (log_size)
        AppendToLog(g_batch_log, log_size);
    if (support_size)
        SupAppendRaw(g_batch_support, support_size);
    if (mark_size)
        WriteWhole(LastPath, g_batch_mark, mark_size);
    if (dropped_log || dropped_support)
    {
        char line[128];
        const int n = std::snprintf(line, sizeof(line),
                                   "logger: queue overflow; omitted %llu developer and %llu support lines\n",
                                   static_cast<unsigned long long>(dropped_log),
                                   static_cast<unsigned long long>(dropped_support));
        if (n > 0)
        {
            AppendToLog(line, static_cast<size_t>(n));
            SupAppendRaw(line, static_cast<size_t>(n));
        }
    }
}

namespace
{
void LogWriter(void *)
{
    for (;;)
    {
        os::SleepThread(TimeSpan::FromMilliSeconds(100));
        LogFlush();
    }
}
} // namespace

void LogInit()
{
    std::scoped_lock lk(g_lock);
    if (R_FAILED(fs::MountSdCard("sdmc")))
    {
        return;
    }
    static_cast<void>(fs::CreateDirectory("sdmc:/config"));
    static_cast<void>(fs::CreateDirectory("sdmc:/config/nxremoteplay"));
    /* the previous boot's log and breadcrumb are kept beside (host_log.prev.txt, host_last.prev.txt): after a
     * crash or a hang the console is rebooted, and the boot that went wrong is the one wanted */
    static_cast<void>(fs::DeleteFile("sdmc:/config/nxremoteplay/host_log.prev.txt"));
    static_cast<void>(fs::RenameFile(LogPath, "sdmc:/config/nxremoteplay/host_log.prev.txt"));
    static_cast<void>(fs::DeleteFile("sdmc:/config/nxremoteplay/host_last.prev.txt"));
    static_cast<void>(fs::RenameFile(LastPath, "sdmc:/config/nxremoteplay/host_last.prev.txt"));
    /* truncate */
    fs::DeleteFile(LogPath);
    fs::CreateFile(LogPath, 0);
    g_ready = true;
    WriteWhole(LastPath, "LogInit\n", 8);
}

void LogMark(const char *what)
{
    {
        std::scoped_lock lk(g_lock);
        if (g_ready && !g_defer)
        {
            char b[128];
            const int n = std::snprintf(b, sizeof(b), "%s\n", what);
            if (n > 0)
            {
                const size_t size = static_cast<size_t>(n) < sizeof(b) ? static_cast<size_t>(n) : sizeof(b) - 1;
                if (g_async)
                {
                    std::memcpy(g_pending_mark, b, size);
                    g_pending_mark_len = size;
                }
                else
                    WriteWhole(LastPath, b, size);
            }
        }
    }
    LogLine("-> %s", what);
}

void LogLine(const char *fmt, ...)
{
    std::scoped_lock lk(g_lock);
    if (!g_ready)
    {
        return;
    }

    char line[512];
    const u64 ms = armTicksToNs(armGetSystemTick()) / UINT64_C(1000000);
    int n = std::snprintf(line, sizeof(line), "[%6llu.%03llu] ", static_cast<unsigned long long>(ms / 1000),
                          static_cast<unsigned long long>(ms % 1000));
    const int pre = n;

    std::va_list ap;
    va_start(ap, fmt);
    n += std::vsnprintf(line + n, sizeof(line) - n - 2, fmt, ap);
    va_end(ap);
    if (n < 0)
    {
        return;
    }
    if (g_sup_ready && SupWorthy(line + pre))
    {
        SupEmitLocked(line + pre);
    } /* the support file gets the lines that matter */
    if (static_cast<size_t>(n) > sizeof(line) - 2)
    {
        n = sizeof(line) - 2;
    }
    line[n++] = '\n';

    if (g_defer)
    {
        if (g_defer_len + static_cast<size_t>(n) <= sizeof(g_defer_buf))
        {
            std::memcpy(g_defer_buf + g_defer_len, line, static_cast<size_t>(n));
            g_defer_len += static_cast<size_t>(n);
        }
        return;
    }
    if (g_async)
        QueueLine(g_pending_log, sizeof(g_pending_log), &g_pending_log_len, &g_dropped_log,
                  line, static_cast<size_t>(n));
    else
        AppendToLog(line, static_cast<size_t>(n));
}

void LogDefer(bool on)
{
    std::scoped_lock lk(g_lock);
    if (on)
    {
        g_defer = true;
        return;
    }
    g_defer = false;
    if (g_ready && g_defer_len != 0)
    {
        if (g_async)
            QueueLine(g_pending_log, sizeof(g_pending_log), &g_pending_log_len, &g_dropped_log,
                      g_defer_buf, g_defer_len);
        else
            AppendToLog(g_defer_buf, g_defer_len);
    }
    g_defer_len = 0;
    if (g_sup_ready && g_sup_defer_len != 0)
    {
        if (g_async)
            QueueLine(g_pending_support, sizeof(g_pending_support), &g_pending_support_len, &g_dropped_support,
                      g_sup_defer, g_sup_defer_len);
        else
            SupAppendRaw(g_sup_defer, g_sup_defer_len);
    }
    g_sup_defer_len = 0;
}

namespace
{
void HeadAppend(size_t *n, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void HeadAppend(size_t *n, const char *fmt, ...)
{
    if (*n >= sizeof(g_sup_head) - 2)
    {
        return;
    }
    std::va_list ap;
    va_start(ap, fmt);
    const int w = std::vsnprintf(g_sup_head + *n, sizeof(g_sup_head) - *n, fmt, ap);
    va_end(ap);
    if (w > 0)
    {
        *n += static_cast<size_t>(w);
        if (*n > sizeof(g_sup_head) - 1)
        {
            *n = sizeof(g_sup_head) - 1;
        }
    }
}
bool HasFile(const char *path)
{
    fs::DirectoryEntryType t;
    return R_SUCCEEDED(fs::GetEntryType(std::addressof(t), path));
}
const char *ModelName(u32 m)
{
    switch (m)
    {
    case SetSysProductModel_Nx:
        return "Switch (Erista)";
    case SetSysProductModel_Copper:
        return "Switch (Erista, simulation)";
    case SetSysProductModel_Iowa:
        return "Switch (Mariko, v2)";
    case SetSysProductModel_Hoag:
        return "Switch Lite";
    case SetSysProductModel_Calcio:
        return "Switch (Mariko, simulation)";
    case SetSysProductModel_Aula:
        return "Switch OLED";
    default:
        return "unknown";
    }
}
} // namespace

void SupportLine(const char *fmt, ...)
{
    char msg[400];
    std::va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n < 0)
    {
        return;
    }
    std::scoped_lock lk(g_lock);
    if (g_sup_ready)
    {
        SupEmitLocked(msg);
    }
}

void SupportPools(const char *when)
{
    char b[380];
    size_t n = 0;
    static const char *const names[4] = {"Application", "Applet", "System", "SystemUnsafe"};
    n += static_cast<size_t>(std::snprintf(b + n, sizeof(b) - n, "memory (%s):", when));
    for (u64 pool = 0; pool < 4; ++pool)
    {
        u64 tot = 0, used = 0;
        const auto r1 = ::ams::svc::GetSystemInfo(
            std::addressof(tot), ::ams::svc::SystemInfoType_TotalPhysicalMemorySize, ::ams::svc::InvalidHandle, pool);
        const auto r2 = ::ams::svc::GetSystemInfo(
            std::addressof(used), ::ams::svc::SystemInfoType_UsedPhysicalMemorySize, ::ams::svc::InvalidHandle, pool);
        if (R_FAILED(r1) || R_FAILED(r2))
        {
            n += static_cast<size_t>(std::snprintf(b + n, sizeof(b) - n, " %s ?", names[pool]));
            continue;
        }
        n += static_cast<size_t>(std::snprintf(b + n, sizeof(b) - n, " %s %llu/%llu MB;", names[pool],
                                               static_cast<unsigned long long>(used >> 20),
                                               static_cast<unsigned long long>(tot >> 20)));
    }
    static const struct
    {
        ::ams::pm::ResourceLimitGroup g;
        const char *name;
    } groups[] = {
        {::ams::pm::ResourceLimitGroup_System, "sysmodule limit"},
        {::ams::pm::ResourceLimitGroup_Applet, "applet limit"},
    };
    for (const auto &g : groups)
    {
        u64 cur = 0, lim = 0;
        if (R_FAILED(::ams::pm::dmnt::AtmosphereGetCurrentLimitInfo(std::addressof(cur), std::addressof(lim), g.g,
                                                                    svc::LimitableResource_PhysicalMemoryMax)))
        {
            continue;
        }
        n += static_cast<size_t>(std::snprintf(b + n, sizeof(b) - n, " %s %llu/%llu MB;", g.name,
                                               static_cast<unsigned long long>(cur >> 20),
                                               static_cast<unsigned long long>(lim >> 20)));
        if (n >= sizeof(b) - 1)
        {
            break;
        }
    }
    SupportLine("%s", b);
}

extern "C" void NetworkSupportLine(const char *fmt, ...)
{
    char line[192];
    std::va_list args;
    va_start(args, fmt);
    const int n = std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (n >= 0)
        SupportLine("network: %s", line);
}

extern "C" void UnifiedSupportLine(const char *line)
{ // called from hidplus (plain C++ side)
    SupportLine("controllers: %s", line);
}

//  USB attachment diagnostics from modes/UsbAuto.c; shows as "usb: <line>" in support.txt.
extern "C" void UnifiedUsbSupportLine(const char *line)
{
    SupportLine("usb: %s", line);
}

void SupportBoot(const char *build)
{
    u32 model = 0;
    char fw[0x20] = "unknown", ams[24] = "unknown";
    if (R_SUCCEEDED(setsysInitialize()))
    {
        SetSysFirmwareVersion v;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(std::addressof(v))))
        {
            std::snprintf(fw, sizeof(fw), "%s", v.display_version);
        }
        SetSysProductModel pm = SetSysProductModel_Invalid;
        if (R_SUCCEEDED(setsysGetProductModel(std::addressof(pm))))
        {
            model = static_cast<u32>(pm);
        }
        setsysExit();
    }
    if (R_SUCCEEDED(splInitialize()))
    {
        u64 cfg = 0;
        if (R_SUCCEEDED(splGetConfig(static_cast<SplConfigItem>(65000), std::addressof(cfg))))
        {
            std::snprintf(ams, sizeof(ams), "%u.%u.%u", static_cast<unsigned>((cfg >> 56) & 0xFF),
                          static_cast<unsigned>((cfg >> 48) & 0xFF), static_cast<unsigned>((cfg >> 40) & 0xFF));
        }
        splExit();
    }
    s64 sd_free = -1, sd_total = -1;
    static_cast<void>(fs::GetFreeSpaceSize(std::addressof(sd_free), "sdmc:/"));
    static_cast<void>(fs::GetTotalSpaceSize(std::addressof(sd_total), "sdmc:/"));
    std::scoped_lock lk(g_lock);
    size_t n = 0;
    HeadAppend(&n, "== NX-Remote-Play support log ==\n");
    HeadAppend(
        &n,
        "This file contains no passwords, Session IDs, owner keys or IP addresses. Send it with a description of what went wrong.\n");
    HeadAppend(&n, "Build: %s\n", build);
    HeadAppend(&n, "Console: %s\n", ModelName(model));
    HeadAppend(&n, "Firmware: %s   Atmosphere: %s\n", fw, ams);
    if (sd_total > 0)
    {
        HeadAppend(&n, "SD card: %llu MB free of %llu MB\n", static_cast<unsigned long long>(sd_free >> 20),
                   static_cast<unsigned long long>(sd_total >> 20));
    }
    HeadAppend(
        &n,
        "Files: capture-helper-module %s, boot2.flag %s, am-patches-folder %s\n",
        HasFile("sdmc:/atmosphere/contents/4E5852656D4D656D/exefs.nsp") ? "yes" : "no",
        HasFile("sdmc:/atmosphere/contents/4E5852656D6F7465/flags/boot2.flag") ? "yes" : "no",
        HasFile("sdmc:/atmosphere/exefs_patches/am") ? "yes" : "no");
    HeadAppend(
        &n,
        "Times are hh:mm:ss since the console was switched on.\n------------------------------------------------------------\n");
    g_sup_head_len = n;
    static_cast<void>(fs::CreateDirectory("sdmc:/config"));
    static_cast<void>(fs::CreateDirectory("sdmc:/config/nxremoteplay"));
    static_cast<void>(fs::DeleteFile(SupPrevPath));
    static_cast<void>(fs::RenameFile(SupPath, SupPrevPath));
    static_cast<void>(fs::DeleteFile(SupPath));
    static_cast<void>(fs::CreateFile(SupPath, 0));
    SupAppendRaw(g_sup_head, g_sup_head_len);
    g_sup_ready = true;
    if (!g_async)
    {
        // os priorities are 0..31; an out-of-range value aborts inside os::CreateThread instead of failing.
        const s32 prio = std::min(os::GetThreadPriority(os::GetCurrentThread()) + 4, os::LowestThreadPriority);
        const Result rc = os::CreateThread(&g_writer_thread, LogWriter, nullptr, g_writer_stack,
                                           sizeof(g_writer_stack), prio);
        if (R_SUCCEEDED(rc))
        {
            os::SetThreadNamePointer(&g_writer_thread, "nxrp.LogWriter");
            g_async = true;
            os::StartThread(&g_writer_thread);
            SupEmitLocked("logger: bounded background writer active (100 ms batches)");
        }
        else
            SupEmitLocked("logger: background writer unavailable; synchronous fallback active");
    }
}

namespace
{
} // namespace

} // namespace ams::mitm::applet
