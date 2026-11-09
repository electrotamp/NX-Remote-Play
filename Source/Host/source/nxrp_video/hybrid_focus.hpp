#pragma once
namespace ams::mitm::applet
{
// Original application's AM focus state; unknown/stale selects composed capture.
void HybridPollFocus();
bool HybridGameFocused();
void HybridLogState();
} // namespace ams::mitm::applet
