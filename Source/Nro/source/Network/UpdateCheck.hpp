#pragma once
#include <string>

// Asks GitHub for the newest published release and compares its tag with this build's version.
// The desktop app, the Android app and the web page make the same check (COMPATIBILITY.md "Update check").
namespace Network::UpdateCheck
{
constexpr const char *ReleasesPage = "https://github.com/ElectroTamp/NX-Remote-Play/releases";

enum class State
{
    NotChecked,
    Checking,
    UpToDate,
    Available,
    Failed
};

// Starts one background check; a call while a check is running does nothing.
void Start();
State Current();
// The newer version (without a leading "v") once Current() is Available.
std::string Latest();
const char *StatusText();
// Cancels a running check and waits for its thread (the app is exiting).
void Join();

// Semantic version order: 1 when a is newer, -1 when older, 0 when equal; -2 when either is not a version.
// A leading "v" and any "+build" suffix are ignored; a pre-release such as -beta.2 sorts before its release.
int Compare(const std::string &a, const std::string &b);
// The newest version tag in a GitHub release-list JSON body (drafts skipped), or "" when there is none.
std::string NewestTag(const std::string &json);
} // namespace Network::UpdateCheck
