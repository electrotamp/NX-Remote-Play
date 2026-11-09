// TCP transport video integration: server machinery derived from the reference project .
// See Provenance/tap-video-integration-source.json and the upstream GPL-2.0 notices.
#include <stratosphere.hpp>
#include "applet_mitm_service.hpp"
#include "applet_mitm_sm.hpp"
#include "applet_mitm_donor.hpp"
#include "applet_mitm_am.hpp"
#include "applet_mitm_log.hpp"
#include "applet_mitm_nv.hpp"
#include "applet_mitm_clk.hpp"
#include "applet_mitm_control.hpp"
#include "applet_mitm_uibinder.hpp"
#include "../nxrp_video.h"
namespace ams
{
namespace
{
enum PortIndex
{
    PortIndex_AppletMitm,
    PortIndex_ViManagerMitm, /* homebrew applications present through vi:m */
    PortIndex_Count,
};

constexpr sm::ServiceName AppletMitmServiceName = sm::ServiceName::Encode("vi:u");
constexpr sm::ServiceName ViManagerMitmServiceName = sm::ServiceName::Encode("vi:m");

struct ServerOptions
{
    static constexpr size_t PointerBufferSize = 0x1000;
    static constexpr size_t MaxDomains = 0;
    static constexpr size_t MaxDomainObjects = 0;
    static constexpr bool CanDeferInvokeRequest = false;
    static constexpr bool CanManageMitmServers = true;
};

/* 16 (8 was tight with a game, a homebrew app and their sub-objects) */
constexpr size_t MaxSessions = 16;

class ServerManager final : public sf::hipc::ServerManager<PortIndex_Count, ServerOptions, MaxSessions>
{
  private:
    virtual Result OnNeedsToAccept(int port_index, Server *server) override;
};

ServerManager g_server_manager;

/* the vi server on several threads. With one, a forwarded call
 * that blocks inside vi - a homebrew app's dequeueBuffer while the
 * on-screen keyboard takes over the screen - held the only thread, and
 * the keyboard applet's own vi:m connection waited on our ShouldMitm
 * answer forever: the keyboard never opened (kingboxnt on GBAtemp;
 * sphaira here, the binder traffic stopped dead at the key press). The
 * same "one blocked call ties up one thread of several" design the grc
 * server below uses. */
constexpr size_t ViThreads = 4;                                                   /* this one and three more */
alignas(os::ThreadStackAlignment) constinit u8 g_vi_stacks[ViThreads - 1][16_KB]; /* 32, 5 used */
constinit os::ThreadType g_vi_threads[ViThreads - 1];
void ViLoopThread(void *)
{
    mitm::applet::LoopProcessSafely(g_server_manager, "vi");
}

/* ----  : dispdrv, for the HOME menu's and applets' frames
 * Research only (probe-ui.txt). Its own server: an applet's
 * dequeueBuffer blocks inside nvnflinger until a buffer is free, so
 * it must not hold a vi thread, and domains are allowed in case an
 * applet converts its session (vi:u's server has none). */
enum UiPortIndex
{
    UiPortIndex_Dispdrv,
    UiPortIndex_Count,
};
constexpr sm::ServiceName DispdrvServiceName = sm::ServiceName::Encode("dispdrv");
struct UiServerOptions
{
    static constexpr size_t PointerBufferSize = 0x3000; /* dispdrv's is 0x3000 */
    static constexpr size_t MaxDomains = 4;
    static constexpr size_t MaxDomainObjects = 16;
    static constexpr bool CanDeferInvokeRequest = false;
    static constexpr bool CanManageMitmServers = true;
};
/* an applet's vi:s root, display service, system display
 * service, relay. Without the probe only the Album's dispdrv session
 * is served; each session here costs its 12 KB pointer buffer in
 * memory, with home_menu off too (: 12 -> 4, one fewer thread) */
constexpr size_t UiMaxSessions = 4;
constexpr size_t UiThreads = 2;
class UiServerManager final : public sf::hipc::ServerManager<UiPortIndex_Count, UiServerOptions, UiMaxSessions>
{
  private:
    virtual Result OnNeedsToAccept(int port_index, Server *server) override;
};
UiServerManager g_ui_server_manager;
alignas(os::ThreadStackAlignment) constinit u8 g_ui_stacks[UiThreads][12_KB];
constinit os::ThreadType g_ui_threads[UiThreads];
void UiLoopThread(void *)
{
    mitm::applet::LoopProcessSafely(g_ui_server_manager, "dispdrv");
}

/* our own dispdrv session first (taken before the mitm exists, so it
 * is the real service), and its pointer buffer: a mitm session's
 * must be at least as large or libstratosphere aborts on accept */
/* GCC 15 guesses the vi server's AllocateServer here (speculative
 * devirtualization) and, the UI server being the smaller of the two,
 * warns about a path that never runs */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
void StartUiBinderMitm()
{
    /* dispdrv also for the Album with home_menu on */
    ::Service dd = {};
    ::Result rc = 0;
    {
        mitm::applet::SmUse sm_dd;
        rc = ::smGetService(std::addressof(dd), "dispdrv");
    }
    if (R_FAILED(rc))
    {
        mitm::applet::LogLine("home menu: dispdrv not opened rc=0x%x - the Album is not streamed", rc);
        return;
    }
    mitm::applet::LogLine("home menu: dispdrv's pointer buffer %#x (ours %#zx)",
                          static_cast<unsigned>(dd.pointer_buffer_size), UiServerOptions::PointerBufferSize);
    // Keep the pointer-buffer metadata after closing the temporary query handle.
    ::Service display_query = dd;
    ::serviceClose(std::addressof(display_query));
    bool any = false;
    if (dd.pointer_buffer_size > UiServerOptions::PointerBufferSize)
    {
        mitm::applet::LogLine("home menu: dispdrv's pointer buffer is larger than ours - the Album is not streamed");
    }
    else
    {
        NXRP_ABORT_UNLESS(g_ui_server_manager.RegisterMitmServer<mitm::applet::UiBinderMitm>(UiPortIndex_Dispdrv,
                                                                                             DispdrvServiceName));
        mitm::applet::LogLine("dispdrv wrapped for %s",
                              "the Album");
        any = true;
    }
    /* the library applets' vi (vi:s here, vi:m in the vi
     * server) - their shared-layer presents name the slot each frame */
    if (!any)
    {
        return;
    }
    const s32 prio = os::GetThreadPriority(os::GetCurrentThread());
    for (size_t i = 0; i < UiThreads; ++i)
    {
        NXRP_ABORT_UNLESS(os::CreateThread(std::addressof(g_ui_threads[i]), UiLoopThread, nullptr, g_ui_stacks[i],
                                           sizeof(g_ui_stacks[i]), prio));
        os::SetThreadNamePointer(std::addressof(g_ui_threads[i]), "applet-mitm.Ui");
        os::StartThread(std::addressof(g_ui_threads[i]));
    }
    mitm::applet::LogLine("home menu: dispdrv server up, %zu threads", UiThreads);
}
#pragma GCC diagnostic pop

Result ServerManager::OnNeedsToAccept(int port_index, Server *server)
{
    std::shared_ptr<::Service> fsrv;
    sm::MitmProcessInfo client_info;
    server->AcknowledgeMitmSession(std::addressof(fsrv), std::addressof(client_info));
    mitm::applet::g_stats.sessions.fetch_add(1);

    mitm::applet::LogLine("OnNeedsToAccept port=%d program=%016llx", port_index,
                          static_cast<unsigned long long>(client_info.program_id.value));

    switch (port_index)
    {
    case PortIndex_AppletMitm: {
        const Result r =
            this->AcceptMitmImpl(server,
                                 sf::CreateSharedObjectEmplaced<mitm::applet::IViRootMitm, mitm::applet::ViRootMitm>(
                                     decltype(fsrv)(fsrv), client_info),
                                 fsrv);
        mitm::applet::LogLine("  AcceptMitmImpl rc=0x%x", r.GetValue());
        R_RETURN(r);
    }
    case PortIndex_ViManagerMitm: {
        const Result r = this->AcceptMitmImpl(
            server,
            sf::CreateSharedObjectEmplaced<mitm::applet::IViManagerRootMitm, mitm::applet::ViManagerRootMitm>(
                decltype(fsrv)(fsrv), client_info),
            fsrv);
        mitm::applet::LogLine("  AcceptMitmImpl (vi:m) rc=0x%x", r.GetValue());
        R_RETURN(r);
    }
        AMS_UNREACHABLE_DEFAULT_CASE();
    }
}

Result UiServerManager::OnNeedsToAccept(int port_index, Server *server)
{
    std::shared_ptr<::Service> fsrv;
    sm::MitmProcessInfo client_info;
    server->AcknowledgeMitmSession(std::addressof(fsrv), std::addressof(client_info));
    mitm::applet::LogLine("%s accept program=%016llx forward pointer_buffer_size=%#x",
                          port_index == UiPortIndex_Dispdrv ? "dispdrv" : "vi:s",
                          static_cast<unsigned long long>(client_info.program_id.value),
                          static_cast<unsigned>(fsrv->pointer_buffer_size));
    switch (port_index)
    {
    case UiPortIndex_Dispdrv:
        R_RETURN(this->AcceptMitmImpl(
            server,
            sf::CreateSharedObjectEmplaced<mitm::applet::IUiBinderMitm, mitm::applet::UiBinderMitm>(
                decltype(fsrv)(fsrv), client_info),
            fsrv));
        AMS_UNREACHABLE_DEFAULT_CASE();
    }
}

alignas(os::ThreadStackAlignment) u8 g_video_main_stack[24_KB];
os::ThreadType g_video_main_thread;
void NxrpMain(void *)
{
    using namespace mitm::applet;
    os::SetThreadNamePointer(os::GetCurrentThread(), "nxrp.Video");
    LogInit();
    // Never let a stale upstream test marker enable the System-pool heap fallback.
    LogLine(
        "NX-Remote-Play host: VIC/NVENC video; USB attachment, watchdog and session diagnostics enabled");
    SupportBoot("NX-Remote-Play host");
    SupportPools("boot");
    g_pmdmnt_rc = ::pmdmntInitialize();
    g_nvstream_qp = 24;
    g_nvstream_gop = 60;
    g_probe_delay_s = 2;
    StartControlService(); // donor transfer-memory  (no upstream viewer UI)
    StartAppWatch();
    StartAppletWatch();
    // System resource group: native session pairs, without the Applet-pool workaround.
    NXRP_ABORT_UNLESS(g_server_manager.RegisterMitmServer<ViRootMitm>(PortIndex_AppletMitm, AppletMitmServiceName));
    NXRP_ABORT_UNLESS(
        g_server_manager.RegisterMitmServer<ViManagerRootMitm>(PortIndex_ViManagerMitm, ViManagerMitmServiceName));
    StartAmMitm(); // returns donor memory before a browser applet needs it
    StartUiBinderMitm();
    const s32 prio = os::GetThreadPriority(os::GetCurrentThread());
    for (size_t i = 0; i < ViThreads - 1; ++i)
    {
        NXRP_ABORT_UNLESS(
            os::CreateThread(&g_vi_threads[i], ViLoopThread, nullptr, g_vi_stacks[i], sizeof(g_vi_stacks[i]), prio));
        os::StartThread(&g_vi_threads[i]);
    }
    StartVicWorker();
    LogMark("NX-Remote-Play video mitms registered; waiting for a TCP viewer and source");
    SupportLine("started: waiting for a viewer");
    LoopProcessSafely(g_server_manager, "nxrp.vi");
}
} // namespace
} // namespace ams
extern "C" void NxrpVideo_Start(void)
{

    using namespace ams;
    const ams::Result rc = os::CreateThread(&g_video_main_thread, NxrpMain, nullptr, g_video_main_stack,
                                            sizeof(g_video_main_stack), os::GetThreadPriority(os::GetCurrentThread()));
    if (R_FAILED(rc))
        return;
    os::StartThread(&g_video_main_thread);
}
extern "C" uint32_t NxrpVideo_SourceEpoch(void)
{
    return ams::mitm::applet::g_source_epoch.load(std::memory_order_relaxed);
}
extern "C" void NxrpVideo_RequestKeyframe(void)
{
    ams::mitm::applet::g_viewer_key_request.store(true, std::memory_order_relaxed);
}
