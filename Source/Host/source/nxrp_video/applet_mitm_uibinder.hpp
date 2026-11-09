// Wrap HOME and applet presentation services while forwarding their display operations.
#pragma once
#include <stratosphere.hpp>
#include <atomic>

#define AMS_UI_BINDER_MITM_INTERFACE_INFO(C, H)                                                                                                                                                                      \
    AMS_SF_METHOD_INFO(C, H, 0, Result, TransactParcel,     (s32 binder_id, u32 code, u32 flags, const sf::InBuffer &parcel_in, const sf::OutBuffer &parcel_out), (binder_id, code, flags, parcel_in, parcel_out))            \
    AMS_SF_METHOD_INFO(C, H, 1, Result, AdjustRefcount,     (s32 binder_id, s32 add, s32 type), (binder_id, add, type))                                                                                                     \
    AMS_SF_METHOD_INFO(C, H, 2, Result, GetNativeHandle,    (s32 binder_id, u32 what), (binder_id, what))                                                                                                                   \
    AMS_SF_METHOD_INFO(C, H, 3, Result, TransactParcelAuto, (s32 binder_id, u32 code, u32 flags, const sf::InAutoSelectBuffer &parcel_in, const sf::OutAutoSelectBuffer &parcel_out), (binder_id, code, flags, parcel_in, parcel_out))

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::applet, IUiBinderMitm, AMS_UI_BINDER_MITM_INTERFACE_INFO, 0x2AB1E145)

/* vi's shared-layer commands (ISystemDisplayService, 22.5.0 sizes).
 *  saw no library applet open dispdrv, yet the controller applet drew
 * into vi's shared-buffer slots 3 and 4: it presents with
 * AcquireSharedFrameBuffer (the slot to draw) and PresentSharedFrameBuffer
 * (the slot drawn). OpenSharedLayer (8250) carries a PID and is left alone. */
namespace ams::mitm::applet
{
struct SharedAcquireOut
{
    u8 fence[0x24];
    s32 slot_indexes[4];
    u32 pad;
    u64 slot;
};
static_assert(sizeof(SharedAcquireOut) == 0x40);
struct SharedPresentIn
{
    u8 fence[0x24];
    s32 crop[4];
    u32 transform;
    s32 swap_interval;
    u32 pad;
    u64 layer;
    u64 slot;
};
static_assert(sizeof(SharedPresentIn) == 0x50);
static_assert(__builtin_offsetof(SharedPresentIn, layer) == 0x40);
} // namespace ams::mitm::applet

#define AMS_UI_SYSDISP_MITM_INTERFACE_INFO(C, H)                                                                                                                   \
    AMS_SF_METHOD_INFO(C, H, 8251, Result, CloseSharedLayer,                    (u64 layer), (layer))                                                            \
    AMS_SF_METHOD_INFO(C, H, 8252, Result, ConnectSharedLayer,                  (u64 layer), (layer))                                                            \
    AMS_SF_METHOD_INFO(C, H, 8253, Result, DisconnectSharedLayer,               (u64 layer), (layer))                                                            \
    AMS_SF_METHOD_INFO(C, H, 8254, Result, AcquireSharedFrameBuffer,            (sf::Out<ams::mitm::applet::SharedAcquireOut> out, u64 layer), (out, layer))   \
    AMS_SF_METHOD_INFO(C, H, 8255, Result, PresentSharedFrameBuffer,            (const ams::mitm::applet::SharedPresentIn &in), (in))                           \
    AMS_SF_METHOD_INFO(C, H, 8256, Result, GetSharedFrameBufferAcquirableEvent, (u64 layer), (layer))

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::applet, IUiSysDisplayMitm, AMS_UI_SYSDISP_MITM_INTERFACE_INFO, 0x2AB1E146)

namespace ams::mitm::applet
{

/* the HOME menu, the overlay and every library applet */
inline bool IsUiProgram(u64 tid)
{
    return tid >= UINT64_C(0x0100000000001000) && tid < UINT64_C(0x0100000000002000);
}

/* a library applet: not the HOME menu or the overlay (they connect at
 * boot), and not the Album's id, which homebrew uses (hbloader) */
inline bool IsLibraryApplet(u64 tid)
{
    return IsUiProgram(tid) && tid != UINT64_C(0x0100000000001000) && tid != UINT64_C(0x010000000000100C) &&
           tid != UINT64_C(0x010000000000100D);
}

class UiSysDisplayMitm : public sf::MitmServiceImplBase
{
  public:
    using MitmServiceImplBase::MitmServiceImplBase;
    static bool ShouldMitm(const sm::MitmProcessInfo &)
    {
        return true;
    }

  public:
    Result CloseSharedLayer(u64 layer);
    Result ConnectSharedLayer(u64 layer);
    Result DisconnectSharedLayer(u64 layer);
    Result AcquireSharedFrameBuffer(sf::Out<SharedAcquireOut> out, u64 layer);
    Result PresentSharedFrameBuffer(const SharedPresentIn &in);
    Result GetSharedFrameBufferAcquirableEvent(u64 layer);
};
static_assert(IsIUiSysDisplayMitm<UiSysDisplayMitm>);

/* the last shared-frame present a library applet made, recorded after vi
 * handled it; count first changes when a present lands (seqlock-free:
 * read count, slot, count again) */
struct SharedPresentState
{
    std::atomic<u32> count;
    std::atomic<s32> slot;
    std::atomic<u64> layer;
    std::atomic<u64> tick;
    std::atomic<u32> acquires;
    std::atomic<s32> acquired_slot;
};
extern SharedPresentState g_shared_present;

/* a binder transaction from the HOME menu or an applet through vi's relay */
void UiNoteTransaction(u64 tid, s32 id, u32 code, const void *in, size_t in_size);

/* dispdrv is wrapped for the probe (every HOME-menu process) and, ,
 * with home_menu on, for the Album (its own buffers, presented like a game's) */

class UiBinderMitm : public sf::MitmServiceImplBase
{
  public:
    using MitmServiceImplBase::MitmServiceImplBase;
    /* program id only - no IPC here (see g_app_pid) */
    static bool ShouldMitm(const sm::MitmProcessInfo &client_info)
    {
        const u64 tid = client_info.program_id.value;
        return (tid == UINT64_C(0x010000000000100D)) && !client_info.override_status.IsHbl();
    }

  public:
    Result TransactParcel(s32 binder_id, u32 code, u32 flags, const sf::InBuffer &parcel_in,
                          const sf::OutBuffer &parcel_out);
    Result AdjustRefcount(s32 binder_id, s32 add, s32 type);
    Result GetNativeHandle(s32 binder_id, u32 what);
    Result TransactParcelAuto(s32 binder_id, u32 code, u32 flags, const sf::InAutoSelectBuffer &parcel_in,
                              const sf::OutAutoSelectBuffer &parcel_out);

  private:
    Result Transact(u32 cmd, s32 binder_id, u32 code, u32 flags, const void *in, size_t in_size, void *out,
                    size_t out_size);
};
static_assert(IsIUiBinderMitm<UiBinderMitm>);

/* our own dispdrv session, for transactions of our own (applet_mitm_uiprobe.cpp) */

/* what the wrapper saw, for the probe's summaries (applet_mitm_uibinder.cpp) */

/* applet_mitm_service.cpp's parcel readers */
s32 ParseParcelSlot(const u8 *p, size_t sz);
bool ParseParcelQueueInput(const u8 *p, size_t sz, u32 *n, u64 out[4], u32 *transform, s32 crop[4]);

} // namespace ams::mitm::applet
