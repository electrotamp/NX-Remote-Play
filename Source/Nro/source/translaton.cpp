#include "translaton.hpp"

namespace Strings
{
ErrorTable Error = {};

void IterateAllStringsForFontBuilding(void *ptr, void (*cb)(void *, std::string_view))
{
    cb(ptr, Error.SysmoduleConnectionTroubleshootLink);
    cb(ptr, Error.FailExitButton);
}

void ResetStringTable()
{
    Error = {};
}
} // namespace Strings
