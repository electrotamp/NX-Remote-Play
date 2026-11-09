#pragma once
#include <string>
#include <vector>

namespace Strings
{
struct ErrorTable
{
    std::string SysmoduleConnectionTroubleshootLink = "For support check the troubleshooting page:";
    std::string FailExitButton = "Click or press + to exit";
};

extern ErrorTable Error;
void IterateAllStringsForFontBuilding(void *, void (*)(void *, std::string_view));
void ResetStringTable();
} // namespace Strings
