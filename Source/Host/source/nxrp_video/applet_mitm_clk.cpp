/*
 * applet-mitm - : engine clocks. See applet_mitm_clk.hpp for the why.
 *
 * Both services are driven with hand-rolled IPC and explicit command ids, the
 * same way this module talks to nvdrv, so nothing depends on libnx's version
 * switches:
 *
 *   clkrst (8.0.0+)     0  OpenSession {u32 module, u32 unk} -> IClkrstSession
 *   IClkrstSession      8  GetClockRate -> u32 hz
 *
 *   mm:u (2.0.0+)       4  InitializeWithId {u32 module, u32 unk, u32 autoclear} -> u32 id
 *                       5  FinalizeWithId   {u32 id}
 *                       6  SetAndWaitWithId {u32 id, u32 hz, s32 timeout}
 *                       7  GetWithId        {u32 id} -> u32 hz
 *
 * Nothing here opens a channel or submits anything. The worst an mm:u request
 * can do is raise an idle engine's clock, which every video player does.
 */
#include "applet_mitm_clk.hpp"
#include "applet_mitm_sm.hpp"
#include "applet_mitm_log.hpp"
#include "applet_mitm_nv.hpp"
#include <cstdio>
#include <cstring>
#include <atomic>

namespace ams::mitm::applet
{


namespace
{

struct Engine
{
    const char *name;
    u32 pcv_module;
};
constexpr Engine SurveyEngines[] = {
    {"VIC", PcvModule_VIC}, /* the control: nvnflinger keeps it running */
    {"NVENC", PcvModule_NVENC}, {"NVJPG", PcvModule_NVJPG}, {"NVDEC", PcvModule_NVDEC}, {"HOST1X", 0x4000002C},
};

struct MmCandidate
{
    u32 id;
    const char *label;
};
constexpr MmCandidate MmCandidates[] = {
    {5, "mm id 5 (libnx: NVENC, nvtegra: NVDEC)"},
    {6, "mm id 6 (libnx: NVDEC)"},
    {7, "mm id 7 (NVJPG)"},
};
constexpr size_t NumMm = sizeof(MmCandidates) / sizeof(MmCandidates[0]);

/* Official code passes INT_MAX kHz, multiplied to Hz in a u32; this is
 * the value that produces (per nvtegra). The service clamps it to the
 * highest rate the current performance mode allows. */
constexpr u32 MaxRequestHz = static_cast<u32>((UINT64_C(1) << 32) - 1000);

constinit os::SdkMutex g_clk_lock;
constinit bool g_mm_open = false;
constinit bool g_holding = false;
constinit Service g_mm_srv = {};
constinit u32 g_req_id[NumMm] = {};
constinit bool g_req_ok[NumMm] = {};

/* ---- clkrst ---- */
::Result ClkrstRate(Service *clk, u32 module, u32 *out_hz)
{
    *out_hz = 0;
    const struct
    {
        u32 module;
        u32 unk;
    } in = {module, 3}; /* 3: what sys-clk passes */
    Service sess = {};
    ::Result rc = serviceDispatchIn(clk, 0, in, .out_num_objects = 1, .out_objects = std::addressof(sess));
    if (R_FAILED(rc))
    {
        return rc;
    }
    rc = serviceDispatchOut(std::addressof(sess), 8, *out_hz);
    serviceClose(std::addressof(sess));
    return rc;
}

/* ---- mm:u ---- */
::Result MmInitialize(u32 module, u32 *out_id)
{
    const struct
    {
        u32 module;
        u32 unk;
        u32 autoclear;
    } in = {module, 8, 0};
    return serviceDispatchInOut(std::addressof(g_mm_srv), 4, in, *out_id);
}
::Result MmFinalize(u32 id)
{
    return serviceDispatchIn(std::addressof(g_mm_srv), 5, id);
}
::Result MmSetAndWait(u32 id, u32 hz, s32 to)
{
    const struct
    {
        u32 id;
        u32 hz;
        s32 timeout;
    } in = {id, hz, to};
    return serviceDispatchIn(std::addressof(g_mm_srv), 6, in);
}


::Result MmGet(u32 id, u32 *out_hz)
{
    return serviceDispatchInOut(std::addressof(g_mm_srv), 7, id, *out_hz);
}

void ReleaseLocked()
{
    if (!g_mm_open)
    {
        return;
    }
    for (size_t i = 0; i < NumMm; ++i)
    {
        if (g_req_ok[i])
        {
            const ::Result rc = MmFinalize(g_req_id[i]);
            LogLine("   clk: release %s rc=0x%x", MmCandidates[i].label, rc);
            g_req_ok[i] = false;
        }
    }
    serviceClose(std::addressof(g_mm_srv));
    g_mm_open = false;
    g_holding = false;
}


const char *EngineName(u32 pcv_module)
{
    for (const auto &e : SurveyEngines)
    {
        if (e.pcv_module == pcv_module)
        {
            return e.name;
        }
    }
    return "?";
}

/* Which mm:u candidates drive which engine. 7 is NVJPG in every
 * source; 5 and 6 are NVENC/NVDEC in some order . */
bool MmDrives(size_t i, u32 pcv_module)
{
    const u32 id = MmCandidates[i].id;
    if (pcv_module == PcvModule_NVJPG)
    {
        return id == 7;
    }
    if (pcv_module == PcvModule_NVENC || pcv_module == PcvModule_NVDEC)
    {
        return id == 5 || id == 6;
    }
    return false;
}






} // namespace

bool ClockRateOf(u32 pcv_module, u32 *out_hz)
{
    *out_hz = 0;
    Service clk = {};
    SmUse sm_clk_204; /* sm only while needed */
    if (R_FAILED(smGetService(std::addressof(clk), "clkrst")))
    {
        return false;
    }
    const ::Result rc = ClkrstRate(std::addressof(clk), pcv_module, out_hz);
    serviceClose(std::addressof(clk));
    return R_SUCCEEDED(rc);
}



bool ClocksHoldForEngines(const char *who)
{
    std::scoped_lock lk(g_clk_lock);
    if (g_holding)
    {
        return true;
    }

    if (!g_mm_open)
    {
        SmUse sm_clk_239; /* sm only while needed */
        const ::Result rc = smGetService(std::addressof(g_mm_srv), "mm:u");
        LogLine("   clk(%s): smGetService(mm:u) rc=0x%x", who, rc);
        if (R_FAILED(rc))
        {
            return false;
        }
        g_mm_open = true;
    }

    bool any = false;
    for (size_t i = 0; i < NumMm; ++i)
    {
        u32 id = 0, before = 0, after = 0;
        const ::Result ri = MmInitialize(MmCandidates[i].id, std::addressof(id));
        if (R_FAILED(ri))
        {
            LogLine("   clk: %s InitializeWithId rc=0x%x", MmCandidates[i].label, ri);
            continue;
        }
        g_req_id[i] = id;
        g_req_ok[i] = true;
        const ::Result rg0 = MmGet(id, std::addressof(before));
        const ::Result rs = MmSetAndWait(id, MaxRequestHz, -1);
        const ::Result rg1 = MmGet(id, std::addressof(after));
        LogLine("   clk: %s req=%u  get=%u Hz (rc=0x%x)  SetAndWait(max) rc=0x%x  -> get=%u Hz (rc=0x%x)",
                MmCandidates[i].label, id, before, rg0, rs, after, rg1);
        any = any || R_SUCCEEDED(rs);
    }
    g_holding = any;
    if (!any)
    {
        ReleaseLocked();
    }
    return any;
}

u32 ClockEnsure(u32 pcv_module, const char *who)
{
    const char *name = EngineName(pcv_module);
    if (!ClocksHoldForEngines(who))
    {
        LogLine("   clk-ensure(%s): no mm:u request could be made", who);
        return 0;
    }
    static const char *const Step[3] = {"re-set max", "set 0, then max", "fresh request"};
    constexpr u32 Attempts = 6;
    for (u32 attempt = 0; attempt < Attempts; ++attempt)
    {
        const u32 step = attempt < 3 ? attempt : 2;
        ::Result rc_last = 0;
        u32 touched = 0;
        {
            std::scoped_lock lk(g_clk_lock);
            for (size_t i = 0; i < NumMm; ++i)
            {
                if (!MmDrives(i, pcv_module))
                {
                    continue;
                }
                if (step == 2 || !g_req_ok[i])
                {
                    if (g_req_ok[i])
                    {
                        static_cast<void>(MmFinalize(g_req_id[i]));
                        g_req_ok[i] = false;
                    }
                    u32 id = 0;
                    rc_last = MmInitialize(MmCandidates[i].id, std::addressof(id));
                    if (R_FAILED(rc_last))
                    {
                        continue;
                    }
                    g_req_id[i] = id;
                    g_req_ok[i] = true;
                }
                else if (step == 1)
                {
                    static_cast<void>(MmSetAndWait(g_req_id[i], 0, -1));
                }
                rc_last = MmSetAndWait(g_req_id[i], MaxRequestHz, -1);
                ++touched;
            }
        }
        u32 hz = 0;
        const bool readable = ClockRateOf(pcv_module, std::addressof(hz));
        LogLine("   clk-ensure(%s) #%u %-15s: %u request(s), SetAndWait rc=0x%x -> clkrst %s=%u Hz%s", who, attempt + 1,
                Step[step], touched, rc_last, name, hz, readable ? "" : " (UNREADABLE)");
        if (readable && hz != 0)
        {
            return hz;
        }
        os::SleepThread(TimeSpan::FromMilliSeconds(50 * (attempt + 1)));
    }
    return 0;
}





} // namespace ams::mitm::applet
