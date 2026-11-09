#include "con_manager.hpp"
#include "../core.h"
#include <cstring>

// the support file lives in the libstratosphere half of the module (its headers clash with core.h): reached through a C
// function
extern "C" void UnifiedSupportLine(const char *line);

// sys-hidplus's debug-log-to-SD-card helper (declared in con_manager.hpp). Its
// original definition lived in sys-hidplus's own main.cpp, which isn't part of this
// merge - NxRemotePlay's main.c is the entrypoint instead. Routed through NxRemotePlay's own
// LOG() macro (writes to /nxremoteplay_log.txt when built with FILE_LOGGING) so
// con_manager.cpp's existing checkpoints ("Starting Network Loop Thread!",
// "Controller initialized!", etc.) are visible for debugging instead of discarded.
int printToFile(const char *myString)
{
    // controller assignment events (applet open/closed, a viewer taking or releasing a player place) also go to
    // the support file; that is where a "the controller picker keeps coming back" report can be read
    if (std::strstr(myString, "LocalMirror") != nullptr && std::strstr(myString, "probe") == nullptr)
        UnifiedSupportLine(myString);
    return 0;
}
