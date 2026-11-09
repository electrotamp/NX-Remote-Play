#include <cstdio>
#include <vector>
#include <string>
#include <filesystem>

#include "PlatformFs.hpp"

namespace fs
{
std::vector<u8> OpenFile(const std::string &name);
void WriteFile(const std::string &name, const std::vector<u8> &data);

// Treat inaccessible paths as absent instead of throwing into UI/config callers.
static inline bool Exists(const std::string &name)
{
    std::error_code ec;
    bool result = std::filesystem::exists(name, ec);
    return !ec && result;
}
static inline void CreateDir(const std::string &path)
{
    std::filesystem::create_directories(path);
}

} // namespace fs