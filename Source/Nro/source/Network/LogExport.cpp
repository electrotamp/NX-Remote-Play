#include "LogExport.hpp"
#include "TicketLogRedaction.hpp"

#include <cstdio>
#include <ctime>
#include <string>
#include <vector>


namespace LogExport
{
const char *const OutputPath = "/NX-Remote-Play-logs.txt";

namespace
{
constexpr const char *ConfigDir = "/config/nxremoteplay/";

struct Piece
{
    const char *file;
    const char *title;
    long maxBytes; // only the end of a longer file is kept
};

// Newest-first value for a person helping: the privacy-safe support file first, then the developer logs.
constexpr Piece Pieces[] = {
    {"host_support.txt", "Console support log (this boot)", 256 * 1024},
    {"host_support.prev.txt", "Console support log (previous boot)", 128 * 1024},
    {"host_last.txt", "Console last-moment breadcrumb (this boot)", 16 * 1024},
    {"host_last.prev.txt", "Console last-moment breadcrumb (previous boot)", 16 * 1024},
    {"host_log.txt", "Console developer log (this boot, end of file)", 256 * 1024},
    {"host_log.prev.txt", "Console developer log (previous boot, end of file)", 128 * 1024},
    {"nro_log.txt", "App connection log (end of file)", 256 * 1024},
};

// "Server replied: OK <id> <ticket>" lines carry a connection ticket; everything else is kept verbatim.
std::string RedactLine(const std::string &line)
{
    static const std::string marker = "Server replied: ";
    const size_t at = line.find(marker);
    if (at == std::string::npos)
        return line;
    const size_t start = at + marker.size();
    return line.substr(0, start) + RedactTicketReply(line.substr(start));
}

void Section(FILE *out, const char *title)
{
    fprintf(out, "\n==== %s ====\n", title);
}

// Copies the last `maxBytes` of the file (from a line boundary) into `out`. Returns the bytes copied, or -1 when the
// file does not exist.
long CopyTail(FILE *out, const std::string &path, long maxBytes)
{
    FILE *in = fopen(path.c_str(), "rb");
    if (!in)
        return -1;
    fseek(in, 0, SEEK_END);
    const long size = ftell(in);
    bool cut = false;
    if (size > maxBytes)
    {
        fseek(in, size - maxBytes, SEEK_SET);
        cut = true;
    }
    else
        fseek(in, 0, SEEK_SET);
    std::vector<char> data(size > 0 ? (size_t)(size > maxBytes ? maxBytes : size) : 0);
    const size_t got = data.empty() ? 0 : fread(data.data(), 1, data.size(), in);
    fclose(in);
    size_t pos = 0;
    if (cut)
    {
        fprintf(out, "(earlier lines left out: %ld bytes)\n", size - (long)got);
        while (pos < got && data[pos] != '\n')
            ++pos; // start on a whole line
        if (pos < got)
            ++pos;
    }
    long written = 0;
    std::string line;
    for (; pos < got; ++pos)
    {
        if (data[pos] == '\n')
        {
            const std::string safe = RedactLine(line) + "\n";
            fwrite(safe.data(), 1, safe.size(), out);
            written += (long)safe.size();
            line.clear();
        }
        else if (data[pos] != '\r')
            line += data[pos];
    }
    if (!line.empty())
    {
        const std::string safe = RedactLine(line) + "\n";
        fwrite(safe.data(), 1, safe.size(), out);
        written += (long)safe.size();
    }
    return written;
}
} // namespace

bool Create(std::string &summary)
{
    FILE *out = fopen(OutputPath, "wb");
    if (!out)
    {
        summary = "Could not create the file on the SD card.";
        return false;
    }
    char stamp[32] = "unknown";
    const time_t now = time(nullptr);
    if (const struct tm *local = localtime(&now))
        strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", local);

    fprintf(out, "== NX-Remote-Play log export ==\n");
    fprintf(out, "Created: %s\nApp: " NXRP_APP_VERSION "\n", stamp);
    fprintf(out, "Send this file with a description of what went wrong. It holds no passwords, Session IDs or owner "
                 "keys.\nThe developer and connection logs contain network addresses (yours and the server's): look through the file before posting it "
                 "publicly.\n");

    int included = 0, missing = 0;
    for (const Piece &piece : Pieces)
    {
        Section(out, piece.title);
        const long n = CopyTail(out, std::string(ConfigDir) + piece.file, piece.maxBytes);
        if (n < 0)
        {
            fprintf(out, "(%s does not exist)\n", piece.file);
            ++missing;
        }
        else
            ++included;
    }
    Section(out, "Server setting (server.txt)");
    if (CopyTail(out, std::string(ConfigDir) + "server.txt", 4096) < 0)
        fprintf(out, "(none: the built-in default server is used)\n");
    Section(out, "Hosting switch (hosting_enabled.txt)");
    if (CopyTail(out, std::string(ConfigDir) + "hosting_enabled.txt", 256) < 0)
        fprintf(out, "(none: hosting is on)\n");

    const bool ok = fflush(out) == 0 && ferror(out) == 0;
    fclose(out);
    if (!ok)
    {
        summary = "Writing the file failed (SD card full or locked?).";
        return false;
    }
    summary = std::to_string(included) + " logs included, " + std::to_string(missing) + " not present.";
    return true;
}
} // namespace LogExport
