#include <switch.h>
#include "util.h"
#include "modes/modes.h"

bool UtilSetConsoleScreenMode(bool on)
{
    Result rc = lblInitialize();

    if (R_SUCCEEDED(rc))
    {
        LblBacklightSwitchStatus lblstatus = LblBacklightSwitchStatus_Disabled;
        if (R_SUCCEEDED(lblGetBacklightSwitchStatus(&lblstatus)))
        {
            if (on && lblstatus == LblBacklightSwitchStatus_Disabled)
                rc = lblSwitchBacklightOn(0);
            else if (!on && lblstatus == LblBacklightSwitchStatus_Enabled)
                rc = lblSwitchBacklightOff(0);
        }
        lblExit();
    }

    return R_SUCCEEDED(rc);
}
