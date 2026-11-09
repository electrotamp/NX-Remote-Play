#include "applet_mitm_am.hpp"
#include "applet_mitm_donor.hpp"
#include "applet_mitm_log.hpp"
#include "applet_mitm_nv.hpp"
#include "hybrid_focus.hpp"
#include "../nxrp_video.h"
#include "../hybrid_policy.hpp"

namespace ams::mitm::applet
{

namespace
{
constinit os::SdkMutex g_focus_lock;
// observe the game's wrapper lifetime without retaining its AM
// object or issuing background IPC against a retiring application.
std::weak_ptr<::Service> g_focus_service;
constinit std::atomic<u64> g_focus_pid{0}, g_focus_tick{0};
constinit std::atomic<u32> g_focus_state{0}, g_focus_rc{0};
u64 g_focus_next_poll = 0, g_focus_mismatch_since = 0;
void TrackFocus(u64 pid, u8 state)
{
    g_focus_pid.store(pid, std::memory_order_relaxed);
    g_focus_state.store(state, std::memory_order_relaxed);
    g_focus_tick.store(armGetSystemTick(), std::memory_order_release);
}
} // namespace
bool HybridGameFocused()
{
    const u64 tick = g_focus_tick.load(std::memory_order_acquire);
    const u64 present = g_app_present_tick.load(std::memory_order_acquire);
    const u64 now = armGetSystemTick();
    return tick && HybridUseGame(g_app_pid.load(), g_focus_pid.load(), g_focus_state.load(), armTicksToNs(now - tick),
                                 present != 0, present ? armTicksToNs(now - present) : 0);
}
void HybridLogState()
{
    const u64 tick = g_focus_tick.load(std::memory_order_acquire);
    LogLine("focus app=%llu observed=%llu state=%u rc=%x age_ms=%llu selected_game=%u",
            static_cast<unsigned long long>(g_app_pid.load()), static_cast<unsigned long long>(g_focus_pid.load()),
            g_focus_state.load(), g_focus_rc.load(),
            static_cast<unsigned long long>(tick ? armTicksToNs(armGetSystemTick() - tick) / 1000000 : 0),
            HybridGameFocused() ? 1u : 0u);
}
void HybridPollFocus()
{

    const u64 now = armTicksToNs(armGetSystemTick());
    if (now < g_focus_next_poll)
        return;
    g_focus_next_poll = now + 100000000ULL;
    std::scoped_lock lk(g_focus_lock);
    if (g_focus_pid.load() != g_app_pid.load() || g_app_pid.load() == 0)
    {
        if (!g_focus_mismatch_since)
            g_focus_mismatch_since = now;
        if (now - g_focus_mismatch_since > 2000000000ULL)
        {
            g_focus_service.reset();
            TrackFocus(0, 0);
        }
        return;
    }
    g_focus_mismatch_since = 0;
    if (g_focus_service.expired())
    {
        g_focus_service.reset();
        TrackFocus(0, 0);
        return;
    }
    // State comes ONLY from the application's own forwarded request below.
    // Refresh observer liveness, not a claim of another AM round trip. The
    // selector still requires a matching PID and fresh game presents.
    TrackFocus(g_focus_pid.load(), static_cast<u8>(g_focus_state.load()));
}

namespace
{

/* Forwarded objects are closed with the wrapper: a domain object of
 * ours must close its id in am's domain too (libstratosphere: "Mitm
 * objects are required to close their associated domain id"). */
std::shared_ptr<::Service> MakeForward(const ::Service &s)
{
    return std::shared_ptr<::Service>(new ::Service(s), [](::Service *p) {
        serviceClose(p);
        delete p;
    });
}

/* Out-objects on a NON-domain session need the patched libstratosphere
 * patch to get their forward service (applet_mitm_service.cpp, vi);
 * on a domain the object id does it. Only set the side channel when
 * it will be consumed, so a stale one never reaches a later object. */
void PendForward(const std::shared_ptr<::Service> &fwd)
{
    if (!serviceIsDomain(fwd.get()) && !serviceIsDomainSubservice(fwd.get()))
    {
        ::ams::sf::impl::g_nxrp_pending_mitm_forward = fwd;
    }
}

/* Library applets that are the console's browser: they need nearly
 * the whole Applet pool (libnx AppletId) */
bool IsBrowserApplet(u32 applet_id)
{
    switch (applet_id)
    {
    case 0x13: /* Web */
    case 0x14: /* Shop (eShop) */
    case 0x17: /* OfflineWeb: manuals, news */
    case 0x18: /* LoginShare */
    case 0x19: /* WifiWebAuth */
    case 0x1A: /* MyPage */
        return true;
    default:
        return false;
    }
}

enum PortIndex
{
    PortIndex_AppletOe,
    PortIndex_Count,
};

constexpr sm::ServiceName AppletOeServiceName = sm::ServiceName::Encode("appletOE");

/* am converts its sessions to domains, and a mitm domain must reserve
 * the very object ids am hands out - from am's one table for every
 * client, so they run high: 32 entries aborted the module (a fatal) as
 * the first game converted (test1). The ids are probed at
 * registration; past half of this, am is left alone. The pointer
 * buffer must be at least am's (smaller aborts too). */
struct AmServerOptions
{
    static constexpr size_t PointerBufferSize = 0x1000;
    static constexpr size_t MaxDomains = 8;           /* 4 - two apps' worth of headroom more */
    static constexpr size_t MaxDomainObjects = 0x200; /* 0x1000 - am's ids ran 34-47 over 15 apps (test9) */
    static constexpr bool CanDeferInvokeRequest = false;
    static constexpr bool CanManageMitmServers = true;
};
constexpr size_t AmMaxSessions = 8; /* was 4, with no room past it (see LoopProcessSafely) */

class AmServerManager final : public sf::hipc::ServerManager<PortIndex_Count, AmServerOptions, AmMaxSessions>
{
  private:
    virtual Result OnNeedsToAccept(int port_index, Server *server) override;
};

AmServerManager g_am_server_manager;

/* two threads: CreateLibraryApplet may wait ~0.1-1.5 s for the
 * stream to stop, and the game's other am calls must not queue behind it */
constexpr size_t AmThreads = 2;
alignas(os::ThreadStackAlignment) constinit u8 g_am_stacks[AmThreads][12_KB]; /* 16, 4 used (test9) */
constinit os::ThreadType g_am_threads[AmThreads];
void AmLoopThread(void *)
{
    LoopProcessSafely(g_am_server_manager, "am");
}

Result AmServerManager::OnNeedsToAccept(int port_index, Server *server)
{
    std::shared_ptr<::Service> fsrv;
    sm::MitmProcessInfo client_info;
    server->AcknowledgeMitmSession(std::addressof(fsrv), std::addressof(client_info));
    AMS_ABORT_UNLESS(port_index == PortIndex_AppletOe);
    /* one line an app start - a slow leak would show here */
    static u32 s_accepted = 0;
    LogLine("am: appletOE of program %016llx wrapped (%u this boot)",
            static_cast<unsigned long long>(client_info.program_id.value), ++s_accepted);
    R_RETURN(this->AcceptMitmImpl(
        server, sf::CreateSharedObjectEmplaced<IAmOeMitm, AmOeMitm>(decltype(fsrv)(fsrv), client_info), fsrv));
}

} // namespace

Result AmOeMitm::OpenApplicationProxy(sf::Out<sf::SharedPointer<IAmProxyMitm>> out,
                                      const sf::ClientProcessId &client_pid, sf::CopyHandle &&process)
{
    ::Service proxy = {};
    const u64 reserved = 0;
    R_TRY(serviceMitmDispatchIn(m_forward_service.get(), 0, reserved, .in_send_pid = true, .in_num_handles = 1,
                                .in_handles = {process.GetOsHandle()}, .out_num_objects = 1,
                                .out_objects = std::addressof(proxy), .override_pid = client_pid.GetValue().value, ));
    const sf::cmif::DomainObjectId target{serviceGetObjectId(std::addressof(proxy))};
    LogLine("am: the app's proxy is am's object %u (our table %zu)", static_cast<unsigned>(target.value),
            AmServerOptions::MaxDomainObjects); /* how high am's ids run */
    auto fwd = MakeForward(proxy);
    PendForward(fwd);
    out.SetValue(sf::CreateSharedObjectEmplaced<IAmProxyMitm, AmProxyMitm>(decltype(fwd)(fwd), m_client_info), target);
    R_SUCCEED();
}

Result AmProxyMitm::GetCommonStateGetter(sf::Out<sf::SharedPointer<IAmCommonMitm>> out)
{
    ::Service common = {};
    R_TRY(serviceDispatch(m_forward_service.get(), 0, .out_num_objects = 1, .out_objects = std::addressof(common)));
    const sf::cmif::DomainObjectId target{serviceGetObjectId(std::addressof(common))};
    auto fwd = MakeForward(common);
    {
        std::scoped_lock lk(g_focus_lock);
        g_focus_service = fwd;
        g_focus_rc.store(0);
        TrackFocus(m_client_info.process_id.value, 0); // unknown until the game asks
        LogLine("passive focus observer pid=%llu; no injected AM query",
                static_cast<unsigned long long>(m_client_info.process_id.value));
    }
    PendForward(fwd);
    out.SetValue(sf::CreateSharedObjectEmplaced<IAmCommonMitm, AmCommonMitm>(decltype(fwd)(fwd), m_client_info),
                 target);
    R_SUCCEED();
}
Result AmCommonMitm::GetCurrentFocusState(sf::Out<u8> out)
{
    u8 state = 0;
    const ::Result rc = serviceDispatchOut(m_forward_service.get(), 9, state);
    if (R_SUCCEEDED(rc))
        out.SetValue(state);
    {
        std::scoped_lock lk(g_focus_lock);
        const auto observed = g_focus_service.lock();
        if (observed && observed.get() == m_forward_service.get())
        {
            const bool changed = g_focus_state.load() != (R_SUCCEEDED(rc) ? state : 0) || g_focus_rc.load() != rc;
            g_focus_rc.store(rc);
            TrackFocus(m_client_info.process_id.value, R_SUCCEEDED(rc) ? state : 0);
            if (changed)
                LogLine("game-owned focus pid=%llu state=%u rc=%x",
                        static_cast<unsigned long long>(m_client_info.process_id.value), state, rc);
        }
    }
    R_RETURN(::ams::Result(rc));
}

Result AmProxyMitm::GetLibraryAppletCreator(sf::Out<sf::SharedPointer<IAmCreatorMitm>> out)
{
    ::Service creator = {};
    R_TRY(serviceDispatch(m_forward_service.get(), 11, .out_num_objects = 1, .out_objects = std::addressof(creator), ));
    const sf::cmif::DomainObjectId target{serviceGetObjectId(std::addressof(creator))};
    auto fwd = MakeForward(creator);
    PendForward(fwd);
    out.SetValue(sf::CreateSharedObjectEmplaced<IAmCreatorMitm, AmCreatorMitm>(decltype(fwd)(fwd), m_client_info),
                 target);
    R_SUCCEED();
}

Result AmCreatorMitm::CreateLibraryApplet(u32 applet_id, u32 mode)
{
    AMS_UNUSED(mode);
    /* the stream stops and the donor's memory is back before am starts it */
    if (IsBrowserApplet(applet_id))
    {
        PrepareForApplet(applet_id);
    }
    /* the request itself goes to am unchanged (from the saved message) */
    R_THROW(sm::mitm::ResultShouldForwardToSession());
}

void StartAmMitm()
{
    /* am's pointer buffer must fit in ours, or accepting a session aborts */
    {
        os::NativeHandle h = os::InvalidNativeHandle;
        if (const Result rc = sm::GetServiceHandle(std::addressof(h), AppletOeServiceName); R_FAILED(rc))
        {
            LogLine("am: appletOE not reachable rc=0x%x - no browser guard while streaming", rc.GetValue());
            return;
        }
        ::Service s = {};
        serviceCreate(std::addressof(s), h);
        const u16 pbs = s.pointer_buffer_size;
        const Result crc = serviceConvertToDomain(std::addressof(s));
        const u32 probe_id = R_SUCCEEDED(crc) ? s.object_id : 0;
        serviceClose(std::addressof(s));
        LogLine(
            "am: appletOE pointer buffer %#x (ours %#zx); a new domain gets object id %u (convert rc=0x%x, our table %zu)",
            pbs, AmServerOptions::PointerBufferSize, probe_id, crc.GetValue(), AmServerOptions::MaxDomainObjects);
        if (pbs > AmServerOptions::PointerBufferSize)
        {
            LogLine("am: pointer buffer too large - not wrapping appletOE");
            return;
        }
        if (R_FAILED(crc) || probe_id == 0 || probe_id > AmServerOptions::MaxDomainObjects / 2)
        {
            LogLine("am: am's object ids do not fit our domain table - not wrapping appletOE");
            return;
        }
    }
    if (const Result rc = g_am_server_manager.RegisterMitmServer<AmOeMitm>(PortIndex_AppletOe, AppletOeServiceName);
        R_FAILED(rc))
    {
        LogLine("am: appletOE mitm not registered rc=0x%x", rc.GetValue());
        return;
    }
    const s32 prio = os::GetThreadPriority(os::GetCurrentThread());
    for (size_t i = 0; i < AmThreads; ++i)
    {
        if (R_FAILED(os::CreateThread(std::addressof(g_am_threads[i]), AmLoopThread, nullptr, g_am_stacks[i],
                                      sizeof(g_am_stacks[i]), prio)))
        {
            LogLine("am: server thread %zu not created", i);
            continue;
        }
        os::SetThreadNamePointer(std::addressof(g_am_threads[i]), "applet-mitm.Am");
        os::StartThread(std::addressof(g_am_threads[i]));
    }
    LogLine("am: appletOE wrapped - a browser applet stops the stream before it starts");
}

} // namespace ams::mitm::applet
