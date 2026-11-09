#include "applet_mitm_control.hpp"
#include "applet_mitm_donor.hpp"
#include <cstdio>
#include "applet_mitm_nv.hpp"
#include "applet_mitm_log.hpp"

namespace ams::mitm::applet
{

namespace
{

class StreamControl
{
  public:

    /* the memory donor (applet_mitm_donor.hpp) */
    Result Donate(u64 size, sf::CopyHandle &&tmem)
    {
        DonorAccept(size, tmem.GetOsHandle());
        tmem.Detach();
        R_SUCCEED();
    }
    /* and the nvdrv session's transfer memory, from the
     * same Applet-pool heap - not 256 KB of our System group */
    Result Donate2(u64 size, u64 nv_size, sf::CopyHandle &&tmem, sf::CopyHandle &&nv_tmem)
    {
        DonorAcceptNv(nv_size, nv_tmem.GetOsHandle());
        nv_tmem.Detach();
        DonorAccept(size, tmem.GetOsHandle());
        tmem.Detach();
        R_SUCCEED();
    }
    Result DonateFailed(u32 rc)
    {
        DonorFailed(rc);
        R_SUCCEED();
    }
    /* the most the donor may take (MB) - less than the usual
     * 42 while the HOME menu streams beside a big applet (Settings) */
    Result GetDonorCap(sf::Out<u32> out_mb)
    {
        out_mb.SetValue(DonorCapMb());
        R_SUCCEED();
    }
};
static_assert(IsIStreamControl<StreamControl>);

using ControlServerOptions = sf::hipc::DefaultServerManagerOptions;
constexpr sm::ServiceName ControlServiceName = sm::ServiceName::Encode("nxrpvid");
constexpr size_t ControlMaxSessions = 3; // donor sessions

sf::hipc::ServerManager<1, ControlServerOptions, ControlMaxSessions> g_control_manager;
constinit sf::UnmanagedServiceObject<IStreamControl, StreamControl> g_control_object;

alignas(os::ThreadStackAlignment) constinit u8 g_control_stack[8_KB]; /* 16, 2 used */
constinit os::ThreadType g_control_thread;

void ControlThread(void *)
{
    LoopProcessSafely(g_control_manager, "nxrp");
}

} // namespace

void StartControlService()
{
    const Result r =
        g_control_manager.RegisterObjectForServer(g_control_object.GetShared(), ControlServiceName, ControlMaxSessions);
    LogLine("nxrp: control service rc=0x%x", r.GetValue());
    if (R_FAILED(r))
    {
        return;
    }
    NXRP_ABORT_UNLESS(os::CreateThread(std::addressof(g_control_thread), ControlThread, nullptr, g_control_stack,
                                       sizeof(g_control_stack), os::GetThreadPriority(os::GetCurrentThread())));
    os::SetThreadNamePointer(std::addressof(g_control_thread), "applet-mitm.Control");
    os::StartThread(std::addressof(g_control_thread));
}

} // namespace ams::mitm::applet
