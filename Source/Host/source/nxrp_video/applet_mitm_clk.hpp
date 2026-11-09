// Request and verify engine clock rates through clkrst and mm:u.
#pragma once
#include <stratosphere.hpp>

namespace ams::mitm::applet
{


/* pcv module ids, as clkrst takes them (libnx pcv.h) */
constexpr u32 PcvModule_VIC = 0x4000002F;
constexpr u32 PcvModule_NVENC = 0x40000030;
constexpr u32 PcvModule_NVJPG = 0x40000031;
constexpr u32 PcvModule_NVDEC = 0x40000032;

/* The clock clkrst reports for one module. False if it could not be read. */
bool ClockRateOf(u32 pcv_module, u32 *out_hz);

/* One log line per engine: the clock clkrst reports right now. */

/* Engine requests can exist while clocks are idle; use ClockEnsure before submitting work. */
bool ClocksHoldForEngines(const char *who);

/* make sure `pcv_module` is clocked RIGHT NOW. Re-applies the mm:u
 * request for that engine and reads the clock back through clkrst,
 * escalating until it is non-zero or the budget is spent:
 *   1. SetAndWait(max) on the existing request
 *   2. SetAndWait(0) then SetAndWait(max) - defeats a cached "no change"
 *   3. Finalize, fresh InitializeWithId, SetAndWait(max)
 * Which step brings it back is itself evidence of how the clock was lost.
 * Returns the rate clkrst reports, 0 if it never came up. */
u32 ClockEnsure(u32 pcv_module, const char *who);

/* Ends the clock watch the survey thread runs while it holds clocks. */

/* Starts the survey thread when "clk" is armed: at t=wait it surveys,
 * holds, surveys again, and (if no engine probe is armed) releases. */

} // namespace ams::mitm::applet
