#pragma once
#include <string>

// "Export logs": gathers the console's own logs into one plain text file on the SD card, so a person can send
// a single file when something goes wrong.
namespace LogExport
{
// The file the export writes (SD card root, overwritten by every export).
extern const char *const OutputPath;

// Builds the file. On success returns true and sets `summary` to a short description of what was included;
// on failure returns false and `summary` holds the reason.
bool Create(std::string &summary);
} // namespace LogExport
