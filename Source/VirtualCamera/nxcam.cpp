// NXRemotePlayCam.dll: a DirectShow video input device ("NX Remote Play Camera") that shows the decoded Switch picture
// the desktop app writes into shared memory (nxcam_shared.h). Written from scratch (the DirectShow base classes are not
// part of the SDK). One output pin, RGB32 1280x720 at 30 or 60 fps. Default registration is per user; DllInstall also
// supports all-users registration from an elevated installer. Nothing here touches the console, the stream protocol or
// the host.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dshow.h>
#include <dvdmedia.h>
#include <initguid.h>
#include <new>
#include <string.h>
#include "nxcam_shared.h"

// {2A8024F0-9BA8-49A0-92A6-62D999786741}
DEFINE_GUID(CLSID_NXRemotePlayCam, 0x2A8024F0, 0x9BA8, 0x49A0, 0x92, 0xA6, 0x62, 0xD9, 0x99, 0x78, 0x67, 0x41);
// KS property set for the pin category (the camera pin is a "capture" pin)
DEFINE_GUID(NX_AMPROPSETID_Pin, 0x9b00f101, 0x1567, 0x11d1, 0xb3, 0xf1, 0x00, 0xaa, 0x00, 0x37, 0x61, 0xc5);
DEFINE_GUID(NX_PIN_CATEGORY_CAPTURE, 0xfb6c4281, 0x0353, 0x11d1, 0x90, 0x5f, 0x00, 0x00, 0xc0, 0xcc, 0x16, 0xba);

static const wchar_t kFilterName[] = L"NX Remote Play Camera";
static HMODULE g_module = nullptr;
static LONG g_objects = 0;
static const int kFps[2] = {30, 60};
static const int kTypeCount = 2;

// ----------------------------------------------------------------------------------------------------------------
// media types
static AM_MEDIA_TYPE *MakeType(int index, bool topDown = false)
{
    int fps = kFps[index < 0 || index >= kTypeCount ? 0 : index];
    auto *mt = (AM_MEDIA_TYPE *)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
    auto *vih = (VIDEOINFOHEADER *)CoTaskMemAlloc(sizeof(VIDEOINFOHEADER));
    if (!mt || !vih)
    {
        CoTaskMemFree(mt);
        CoTaskMemFree(vih);
        return nullptr;
    }
    memset(mt, 0, sizeof(*mt));
    memset(vih, 0, sizeof(*vih));
    vih->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    vih->bmiHeader.biWidth = NXCAM_W;
    vih->bmiHeader.biHeight =
        topDown ? -(LONG)NXCAM_H : (LONG)NXCAM_H; // positive = bottom-up DIB (the usual RGB32 form)
    vih->bmiHeader.biPlanes = 1;
    vih->bmiHeader.biBitCount = 32;
    vih->bmiHeader.biCompression = BI_RGB;
    vih->bmiHeader.biSizeImage = NXCAM_SLOT_BYTES;
    vih->AvgTimePerFrame = 10000000 / fps;
    vih->dwBitRate = (DWORD)((uint64_t)NXCAM_SLOT_BYTES * 8 * fps);
    mt->majortype = MEDIATYPE_Video;
    mt->subtype = MEDIASUBTYPE_RGB32;
    mt->bFixedSizeSamples = TRUE;
    mt->lSampleSize = NXCAM_SLOT_BYTES;
    mt->formattype = FORMAT_VideoInfo;
    mt->cbFormat = sizeof(VIDEOINFOHEADER);
    mt->pbFormat = (BYTE *)vih;
    return mt;
}
static void FreeType(AM_MEDIA_TYPE *mt)
{
    if (!mt)
        return;
    if (mt->pbFormat)
        CoTaskMemFree(mt->pbFormat);
    if (mt->pUnk)
        mt->pUnk->Release();
    CoTaskMemFree(mt);
}
static HRESULT CopyType(AM_MEDIA_TYPE *dst, const AM_MEDIA_TYPE *src)
{
    *dst = *src;
    dst->pbFormat = nullptr;
    if (src->cbFormat && src->pbFormat)
    {
        dst->pbFormat = (BYTE *)CoTaskMemAlloc(src->cbFormat);
        if (!dst->pbFormat)
            return E_OUTOFMEMORY;
        memcpy(dst->pbFormat, src->pbFormat, src->cbFormat);
    }
    if (dst->pUnk)
        dst->pUnk->AddRef();
    return S_OK;
}
// Accepts only RGB32 1280x720 (either row order). fps/topDown report what the type asks for.
static bool AcceptType(const AM_MEDIA_TYPE *mt, int *fpsIndex = nullptr, bool *topDown = nullptr)
{
    if (!mt)
        return false;
    if (mt->majortype != MEDIATYPE_Video || mt->subtype != MEDIASUBTYPE_RGB32)
        return false;
    if (mt->formattype != FORMAT_VideoInfo || !mt->pbFormat || mt->cbFormat < sizeof(VIDEOINFOHEADER))
        return false;
    auto *vih = (const VIDEOINFOHEADER *)mt->pbFormat;
    if (vih->bmiHeader.biWidth != (LONG)NXCAM_W)
        return false;
    LONG h = vih->bmiHeader.biHeight;
    if (h != (LONG)NXCAM_H && h != -(LONG)NXCAM_H)
        return false;
    if (vih->bmiHeader.biBitCount != 32)
        return false;
    if (fpsIndex)
    {
        *fpsIndex = 0;
        if (vih->AvgTimePerFrame > 0 && vih->AvgTimePerFrame <= 10000000 / 45)
            *fpsIndex = 1; // 45 fps or faster asks for 60
    }
    if (topDown)
        *topDown = h < 0;
    return true;
}

class NXFilter;

// ----------------------------------------------------------------------------------------------------------------
// enumerators
class NXEnumMediaTypes : public IEnumMediaTypes
{
    LONG ref_ = 1;
    int pos_;

  public:
    explicit NXEnumMediaTypes(int pos = 0) : pos_(pos)
    {
        InterlockedIncrement(&g_objects);
    }
    virtual ~NXEnumMediaTypes()
    {
        InterlockedDecrement(&g_objects);
    }
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_IEnumMediaTypes)
        {
            *ppv = static_cast<IEnumMediaTypes *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override
    {
        return InterlockedIncrement(&ref_);
    }
    STDMETHODIMP_(ULONG) Release() override
    {
        LONG r = InterlockedDecrement(&ref_);
        if (!r)
            delete this;
        return r;
    }
    STDMETHODIMP Next(ULONG count, AM_MEDIA_TYPE **types, ULONG *fetched) override
    {
        if (!types || (count > 1 && !fetched))
            return E_POINTER;
        ULONG got = 0;
        while (got < count && pos_ < kTypeCount)
        {
            types[got] = MakeType(pos_);
            if (!types[got])
                break;
            ++got;
            ++pos_;
        }
        if (fetched)
            *fetched = got;
        return got == count ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG count) override
    {
        pos_ += (int)count;
        return pos_ <= kTypeCount ? S_OK : S_FALSE;
    }
    STDMETHODIMP Reset() override
    {
        pos_ = 0;
        return S_OK;
    }
    STDMETHODIMP Clone(IEnumMediaTypes **out) override
    {
        if (!out)
            return E_POINTER;
        *out = new (std::nothrow) NXEnumMediaTypes(pos_);
        return *out ? S_OK : E_OUTOFMEMORY;
    }
};

// ---------------------------------------------------------------------------------------------------------------- the
// output pin
class NXPin : public IPin, public IAMStreamConfig, public IKsPropertySet
{
    NXFilter *filter_;
    IPin *peer_ = nullptr;
    IMemInputPin *mem_ = nullptr;
    IMemAllocator *alloc_ = nullptr;
    AM_MEDIA_TYPE type_; // the connected type (valid while peer_)
    int fpsIndex_ = 0;   // format picked through IAMStreamConfig::SetFormat / connection
    bool topDown_ = false;
    HANDLE thread_ = nullptr;
    HANDLE stop_ = nullptr;
    HANDLE mapping_ = nullptr;
    uint8_t *view_ = nullptr;
    bool counted_ = false;
    unsigned mapRetry_ = 0;
    uint8_t *waiting_ = nullptr; // pre-drawn "waiting" picture (top-down BGRA), built on first use
    void BuildWaiting();
    volatile LONG64 streamStart_ = 0; // graph time at Run (REFERENCE_TIME)
    CRITICAL_SECTION cs_;

    static DWORD WINAPI ThreadMain(LPVOID p)
    {
        static_cast<NXPin *>(p)->Stream();
        return 0;
    }
    void Stream();
    void Fill(uint8_t *dst);
    void CloseMap();
    void OpenMap();

  public:
    explicit NXPin(NXFilter *f) : filter_(f)
    {
        memset(&type_, 0, sizeof(type_));
        InitializeCriticalSection(&cs_);
    }
    virtual ~NXPin()
    {
        StopStreaming();
        DisconnectInternal();
        CloseMap();
        delete[] waiting_;
        DeleteCriticalSection(&cs_);
    }
    void SetStreamStart(REFERENCE_TIME t)
    {
        InterlockedExchange64(&streamStart_, t);
    }
    HRESULT StartStreaming();
    void StopStreaming();
    void DisconnectInternal()
    {
        if (mem_)
        {
            mem_->Release();
            mem_ = nullptr;
        }
        if (alloc_)
        {
            alloc_->Release();
            alloc_ = nullptr;
        }
        if (peer_)
        {
            peer_->Release();
            peer_ = nullptr;
        }
        if (type_.pbFormat)
        {
            CoTaskMemFree(type_.pbFormat);
        }
        memset(&type_, 0, sizeof(type_));
    }
    bool Connected() const
    {
        return peer_ != nullptr;
    }

    // IUnknown (the pin lives and dies with its filter)
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;
    // IPin
    STDMETHODIMP Connect(IPin *receive, const AM_MEDIA_TYPE *pmt) override;
    STDMETHODIMP ReceiveConnection(IPin *, const AM_MEDIA_TYPE *) override
    {
        return E_UNEXPECTED;
    }
    STDMETHODIMP Disconnect() override;
    STDMETHODIMP ConnectedTo(IPin **pin) override
    {
        if (!pin)
            return E_POINTER;
        EnterCriticalSection(&cs_);
        *pin = peer_;
        if (peer_)
            peer_->AddRef();
        LeaveCriticalSection(&cs_);
        return *pin ? S_OK : VFW_E_NOT_CONNECTED;
    }
    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE *mt) override
    {
        if (!mt)
            return E_POINTER;
        EnterCriticalSection(&cs_);
        HRESULT hr = peer_ ? CopyType(mt, &type_) : VFW_E_NOT_CONNECTED;
        LeaveCriticalSection(&cs_);
        return hr;
    }
    STDMETHODIMP QueryPinInfo(PIN_INFO *info) override;
    STDMETHODIMP QueryDirection(PIN_DIRECTION *dir) override
    {
        if (!dir)
            return E_POINTER;
        *dir = PINDIR_OUTPUT;
        return S_OK;
    }
    STDMETHODIMP QueryId(LPWSTR *id) override
    {
        if (!id)
            return E_POINTER;
        *id = (LPWSTR)CoTaskMemAlloc(sizeof(L"Capture"));
        if (!*id)
            return E_OUTOFMEMORY;
        memcpy(*id, L"Capture", sizeof(L"Capture"));
        return S_OK;
    }
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE *pmt) override
    {
        return AcceptType(pmt) ? S_OK : S_FALSE;
    }
    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes **e) override
    {
        if (!e)
            return E_POINTER;
        *e = new (std::nothrow) NXEnumMediaTypes();
        return *e ? S_OK : E_OUTOFMEMORY;
    }
    STDMETHODIMP QueryInternalConnections(IPin **, ULONG *) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP EndOfStream() override
    {
        return E_UNEXPECTED;
    }
    STDMETHODIMP BeginFlush() override
    {
        return E_UNEXPECTED;
    }
    STDMETHODIMP EndFlush() override
    {
        return E_UNEXPECTED;
    }
    STDMETHODIMP NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) override
    {
        return E_UNEXPECTED;
    }
    // IAMStreamConfig
    STDMETHODIMP SetFormat(AM_MEDIA_TYPE *pmt) override
    {
        int idx;
        bool td;
        if (!pmt)
        {
            EnterCriticalSection(&cs_);
            fpsIndex_ = 0;
            topDown_ = false;
            LeaveCriticalSection(&cs_);
            return S_OK;
        }
        if (!AcceptType(pmt, &idx, &td))
            return VFW_E_INVALIDMEDIATYPE;
        EnterCriticalSection(&cs_);
        HRESULT hr = S_OK;
        if (peer_)
            hr = VFW_E_NOT_STOPPED; // a connected pin keeps its format (the app reconnects)
        else
        {
            fpsIndex_ = idx;
            topDown_ = td;
        }
        LeaveCriticalSection(&cs_);
        return hr;
    }
    STDMETHODIMP GetFormat(AM_MEDIA_TYPE **pmt) override
    {
        if (!pmt)
            return E_POINTER;
        EnterCriticalSection(&cs_);
        *pmt = MakeType(fpsIndex_, topDown_);
        LeaveCriticalSection(&cs_);
        return *pmt ? S_OK : E_OUTOFMEMORY;
    }
    STDMETHODIMP GetNumberOfCapabilities(int *count, int *size) override
    {
        if (!count || !size)
            return E_POINTER;
        *count = kTypeCount;
        *size = sizeof(VIDEO_STREAM_CONFIG_CAPS);
        return S_OK;
    }
    STDMETHODIMP GetStreamCaps(int index, AM_MEDIA_TYPE **pmt, BYTE *caps) override
    {
        if (!pmt || !caps)
            return E_POINTER;
        if (index < 0 || index >= kTypeCount)
            return S_FALSE;
        *pmt = MakeType(index);
        if (!*pmt)
            return E_OUTOFMEMORY;
        auto *c = (VIDEO_STREAM_CONFIG_CAPS *)caps;
        memset(c, 0, sizeof(*c));
        c->guid = FORMAT_VideoInfo;
        c->InputSize.cx = NXCAM_W;
        c->InputSize.cy = NXCAM_H;
        c->MinCroppingSize = c->MaxCroppingSize = c->InputSize;
        c->MinOutputSize = c->MaxOutputSize = c->InputSize;
        c->MinBitsPerSecond = c->MaxBitsPerSecond = ((VIDEOINFOHEADER *)(*pmt)->pbFormat)->dwBitRate;
        c->MinFrameInterval = c->MaxFrameInterval = 10000000 / kFps[index];
        return S_OK;
    }
    // IKsPropertySet
    STDMETHODIMP Set(REFGUID, DWORD, LPVOID, DWORD, LPVOID, DWORD) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP Get(REFGUID set, DWORD id, LPVOID, DWORD, LPVOID data, DWORD size, DWORD *returned) override
    {
        if (set != NX_AMPROPSETID_Pin)
            return E_PROP_SET_UNSUPPORTED;
        if (id != 0)
            return E_PROP_ID_UNSUPPORTED; // AMPROPERTY_PIN_CATEGORY = 0
        if (!data && !returned)
            return E_POINTER;
        if (returned)
            *returned = sizeof(GUID);
        if (!data)
            return S_OK;
        if (size < sizeof(GUID))
            return E_UNEXPECTED;
        *(GUID *)data = NX_PIN_CATEGORY_CAPTURE;
        return S_OK;
    }
    STDMETHODIMP QuerySupported(REFGUID set, DWORD id, DWORD *support) override
    {
        if (set != NX_AMPROPSETID_Pin)
            return E_PROP_SET_UNSUPPORTED;
        if (id != 0)
            return E_PROP_ID_UNSUPPORTED;
        if (support)
            *support = KSPROPERTY_SUPPORT_GET;
        return S_OK;
    }
};

class NXEnumPins : public IEnumPins
{
    LONG ref_ = 1;
    NXFilter *filter_;
    int pos_;

  public:
    NXEnumPins(NXFilter *f, int pos);
    virtual ~NXEnumPins();
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_IEnumPins)
        {
            *ppv = static_cast<IEnumPins *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override
    {
        return InterlockedIncrement(&ref_);
    }
    STDMETHODIMP_(ULONG) Release() override
    {
        LONG r = InterlockedDecrement(&ref_);
        if (!r)
            delete this;
        return r;
    }
    STDMETHODIMP Next(ULONG count, IPin **pins, ULONG *fetched) override;
    STDMETHODIMP Skip(ULONG count) override
    {
        pos_ += (int)count;
        return pos_ <= 1 ? S_OK : S_FALSE;
    }
    STDMETHODIMP Reset() override
    {
        pos_ = 0;
        return S_OK;
    }
    STDMETHODIMP Clone(IEnumPins **out) override;
};

// ---------------------------------------------------------------------------------------------------------------- the
// filter
class NXFilter : public IBaseFilter, public IAMFilterMiscFlags
{
    LONG ref_ = 1;
    CRITICAL_SECTION cs_;
    FILTER_STATE state_ = State_Stopped;
    IReferenceClock *clock_ = nullptr;
    IFilterGraph *graph_ = nullptr; // not referenced (DirectShow convention)
    wchar_t name_[128];

  public:
    NXPin pin;
    NXFilter() : pin(this)
    {
        InitializeCriticalSection(&cs_);
        wcscpy(name_, kFilterName);
        InterlockedIncrement(&g_objects);
    }
    virtual ~NXFilter()
    {
        if (clock_)
            clock_->Release();
        DeleteCriticalSection(&cs_);
        InterlockedDecrement(&g_objects);
    }
    IReferenceClock *ClockAddRef()
    {
        EnterCriticalSection(&cs_);
        IReferenceClock *c = clock_;
        if (c)
            c->AddRef();
        LeaveCriticalSection(&cs_);
        return c;
    }
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv)
            return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IPersist || riid == IID_IMediaFilter || riid == IID_IBaseFilter)
            *ppv = static_cast<IBaseFilter *>(this);
        else if (riid == IID_IAMFilterMiscFlags)
            *ppv = static_cast<IAMFilterMiscFlags *>(this);
        else
        {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override
    {
        return InterlockedIncrement(&ref_);
    }
    STDMETHODIMP_(ULONG) Release() override
    {
        LONG r = InterlockedDecrement(&ref_);
        if (!r)
            delete this;
        return r;
    }
    // IPersist / IMediaFilter
    STDMETHODIMP GetClassID(CLSID *id) override
    {
        if (!id)
            return E_POINTER;
        *id = CLSID_NXRemotePlayCam;
        return S_OK;
    }
    STDMETHODIMP Stop() override
    {
        EnterCriticalSection(&cs_);
        if (state_ != State_Stopped)
        {
            pin.StopStreaming();
            state_ = State_Stopped;
        }
        LeaveCriticalSection(&cs_);
        return S_OK;
    }
    STDMETHODIMP Pause() override
    {
        EnterCriticalSection(&cs_);
        HRESULT hr = S_OK;
        if (state_ == State_Stopped && pin.Connected())
            hr = pin.StartStreaming();
        if (SUCCEEDED(hr))
            state_ = State_Paused;
        LeaveCriticalSection(&cs_);
        return hr;
    }
    STDMETHODIMP Run(REFERENCE_TIME start) override
    {
        EnterCriticalSection(&cs_);
        HRESULT hr = S_OK;
        pin.SetStreamStart(start);
        if (state_ == State_Stopped && pin.Connected())
            hr = pin.StartStreaming();
        if (SUCCEEDED(hr))
            state_ = State_Running;
        LeaveCriticalSection(&cs_);
        return hr;
    }
    STDMETHODIMP GetState(DWORD, FILTER_STATE *state) override
    {
        if (!state)
            return E_POINTER;
        EnterCriticalSection(&cs_);
        *state = state_;
        HRESULT hr = state_ == State_Paused ? VFW_S_CANT_CUE : S_OK; // a live source cannot preroll
        LeaveCriticalSection(&cs_);
        return hr;
    }
    STDMETHODIMP SetSyncSource(IReferenceClock *clock) override
    {
        EnterCriticalSection(&cs_);
        if (clock)
            clock->AddRef();
        if (clock_)
            clock_->Release();
        clock_ = clock;
        LeaveCriticalSection(&cs_);
        return S_OK;
    }
    STDMETHODIMP GetSyncSource(IReferenceClock **clock) override
    {
        if (!clock)
            return E_POINTER;
        *clock = ClockAddRef();
        return S_OK;
    }
    // IBaseFilter
    STDMETHODIMP EnumPins(IEnumPins **e) override
    {
        if (!e)
            return E_POINTER;
        *e = new (std::nothrow) NXEnumPins(this, 0);
        return *e ? S_OK : E_OUTOFMEMORY;
    }
    STDMETHODIMP FindPin(LPCWSTR id, IPin **out) override
    {
        if (!id || !out)
            return E_POINTER;
        if (_wcsicmp(id, L"Capture") != 0)
        {
            *out = nullptr;
            return VFW_E_NOT_FOUND;
        }
        *out = static_cast<IPin *>(&pin);
        pin.AddRef();
        return S_OK;
    }
    STDMETHODIMP QueryFilterInfo(FILTER_INFO *info) override
    {
        if (!info)
            return E_POINTER;
        EnterCriticalSection(&cs_);
        wcsncpy(info->achName, name_, MAX_FILTER_NAME - 1);
        info->achName[MAX_FILTER_NAME - 1] = 0;
        info->pGraph = graph_;
        if (graph_)
            graph_->AddRef();
        LeaveCriticalSection(&cs_);
        return S_OK;
    }
    STDMETHODIMP JoinFilterGraph(IFilterGraph *graph, LPCWSTR name) override
    {
        EnterCriticalSection(&cs_);
        graph_ = graph;
        if (name)
        {
            wcsncpy(name_, name, 127);
            name_[127] = 0;
        }
        else
            wcscpy(name_, kFilterName);
        LeaveCriticalSection(&cs_);
        return S_OK;
    }
    STDMETHODIMP QueryVendorInfo(LPWSTR *) override
    {
        return E_NOTIMPL;
    }
    // IAMFilterMiscFlags
    STDMETHODIMP_(ULONG) GetMiscFlags() override
    {
        return AM_FILTER_MISC_FLAGS_IS_SOURCE;
    }
};

NXEnumPins::NXEnumPins(NXFilter *f, int pos) : filter_(f), pos_(pos)
{
    filter_->AddRef();
}
NXEnumPins::~NXEnumPins()
{
    filter_->Release();
}
STDMETHODIMP NXEnumPins::Next(ULONG count, IPin **pins, ULONG *fetched)
{
    if (!pins || (count > 1 && !fetched))
        return E_POINTER;
    ULONG got = 0;
    if (count > 0 && pos_ < 1)
    {
        pins[0] = static_cast<IPin *>(&filter_->pin);
        filter_->pin.AddRef();
        got = 1;
        pos_ = 1;
    }
    if (fetched)
        *fetched = got;
    return got == count ? S_OK : S_FALSE;
}
STDMETHODIMP NXEnumPins::Clone(IEnumPins **out)
{
    if (!out)
        return E_POINTER;
    *out = new (std::nothrow) NXEnumPins(filter_, pos_);
    return *out ? S_OK : E_OUTOFMEMORY;
}

// ---------------------------------------------------------------------------------------------------------------- pin
// methods
STDMETHODIMP NXPin::QueryInterface(REFIID riid, void **ppv)
{
    if (!ppv)
        return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IPin)
        *ppv = static_cast<IPin *>(this);
    else if (riid == IID_IAMStreamConfig)
        *ppv = static_cast<IAMStreamConfig *>(this);
    else if (riid == IID_IKsPropertySet)
        *ppv = static_cast<IKsPropertySet *>(this);
    else
    {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}
STDMETHODIMP_(ULONG) NXPin::AddRef()
{
    return filter_->AddRef();
}
STDMETHODIMP_(ULONG) NXPin::Release()
{
    return filter_->Release();
}
STDMETHODIMP NXPin::QueryPinInfo(PIN_INFO *info)
{
    if (!info)
        return E_POINTER;
    info->pFilter = static_cast<IBaseFilter *>(filter_);
    filter_->AddRef();
    info->dir = PINDIR_OUTPUT;
    wcscpy(info->achName, L"Capture");
    return S_OK;
}

STDMETHODIMP NXPin::Connect(IPin *receive, const AM_MEDIA_TYPE *pmt)
{
    if (!receive)
        return E_POINTER;
    EnterCriticalSection(&cs_);
    HRESULT hr = S_OK;
    if (peer_)
        hr = VFW_E_ALREADY_CONNECTED;
    // try the requested type, else our own types in order (the format chosen with SetFormat first)
    AM_MEDIA_TYPE *chosen = nullptr;
    if (SUCCEEDED(hr))
    {
        auto tryType = [&](const AM_MEDIA_TYPE *t) -> bool {
            if (!AcceptType(t))
                return false;
            if (FAILED(receive->ReceiveConnection(static_cast<IPin *>(this), t)))
                return false;
            return true;
        };
        if (pmt && pmt->majortype != GUID_NULL && pmt->pbFormat)
        {
            if (tryType(pmt))
            {
                chosen = (AM_MEDIA_TYPE *)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
                if (chosen)
                    CopyType(chosen, pmt);
            }
            else
                hr = VFW_E_TYPE_NOT_ACCEPTED;
        }
        else
        {
            for (int k = 0; k < kTypeCount * 2 && !chosen; ++k)
            {
                int index = (fpsIndex_ + (k % kTypeCount)) % kTypeCount;
                bool td = k < kTypeCount ? topDown_ : !topDown_;
                AM_MEDIA_TYPE *t = MakeType(index, td);
                if (t && tryType(t))
                    chosen = t;
                else
                    FreeType(t);
            }
            if (!chosen)
                hr = VFW_E_NO_ACCEPTABLE_TYPES;
        }
    }
    if (SUCCEEDED(hr) && chosen)
    {
        // allocator: the receiver's own if it offers one, else the system memory allocator
        IMemInputPin *mem = nullptr;
        hr = receive->QueryInterface(IID_IMemInputPin, (void **)&mem);
        IMemAllocator *alloc = nullptr;
        if (SUCCEEDED(hr))
        {
            hr = mem->GetAllocator(&alloc);
            if (FAILED(hr))
                hr = CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER, IID_IMemAllocator,
                                      (void **)&alloc);
        }
        if (SUCCEEDED(hr))
        {
            ALLOCATOR_PROPERTIES want = {}, got = {};
            if (FAILED(mem->GetAllocatorRequirements(&want)))
                memset(&want, 0, sizeof(want));
            if (want.cBuffers < 3)
                want.cBuffers = 3;
            if (want.cbBuffer < (long)NXCAM_SLOT_BYTES)
                want.cbBuffer = (long)NXCAM_SLOT_BYTES;
            if (want.cbAlign < 1)
                want.cbAlign = 1;
            hr = alloc->SetProperties(&want, &got);
            if (SUCCEEDED(hr) && got.cbBuffer < (long)NXCAM_SLOT_BYTES)
                hr = E_FAIL;
            if (SUCCEEDED(hr))
                hr = mem->NotifyAllocator(alloc, FALSE);
        }
        if (SUCCEEDED(hr))
        {
            AcceptType(chosen, &fpsIndex_, &topDown_);
            peer_ = receive;
            peer_->AddRef();
            mem_ = mem;
            mem = nullptr;
            alloc_ = alloc;
            alloc = nullptr;
            type_ = *chosen;
            chosen->pbFormat = nullptr; // keep the format block
        }
        if (mem)
            mem->Release();
        if (alloc)
            alloc->Release();
        if (FAILED(hr))
            receive->Disconnect();
    }
    if (chosen)
    {
        if (chosen->pbFormat)
            CoTaskMemFree(chosen->pbFormat);
        CoTaskMemFree(chosen);
    }
    LeaveCriticalSection(&cs_);
    return hr;
}

STDMETHODIMP NXPin::Disconnect()
{
    EnterCriticalSection(&cs_);
    HRESULT hr = peer_ ? S_OK : S_FALSE;
    if (thread_)
        hr = VFW_E_NOT_STOPPED; // still streaming
    else
        DisconnectInternal();
    LeaveCriticalSection(&cs_);
    return hr;
}

// ----------------------------------------------------------------------------------------------------------------
// streaming
HRESULT NXPin::StartStreaming()
{
    EnterCriticalSection(&cs_);
    HRESULT hr = S_OK;
    if (!peer_ || !alloc_ || !mem_)
        hr = VFW_E_NOT_CONNECTED;
    else if (!thread_)
    {
        hr = alloc_->Commit();
        if (SUCCEEDED(hr))
        {
            stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            thread_ = CreateThread(nullptr, 0, ThreadMain, this, 0, nullptr);
            if (!thread_)
            {
                CloseHandle(stop_);
                stop_ = nullptr;
                alloc_->Decommit();
                hr = E_FAIL;
            }
        }
    }
    LeaveCriticalSection(&cs_);
    return hr;
}
void NXPin::StopStreaming()
{
    HANDLE t = nullptr, s = nullptr;
    EnterCriticalSection(&cs_);
    t = thread_;
    s = stop_;
    thread_ = nullptr;
    stop_ = nullptr;
    LeaveCriticalSection(&cs_);
    if (t)
    {
        SetEvent(s);
        if (alloc_)
            alloc_->Decommit(); // releases a GetBuffer that is waiting for a free buffer
        WaitForSingleObject(t, 5000);
        CloseHandle(t);
        CloseHandle(s);
    }
    CloseMap();
}
void NXPin::CloseMap()
{
    if (counted_ && view_)
    {
        InterlockedDecrement((volatile LONG *)&((NxCamHeader *)view_)->readers);
    }
    counted_ = false;
    if (view_)
    {
        UnmapViewOfFile(view_);
        view_ = nullptr;
    }
    if (mapping_)
    {
        CloseHandle(mapping_);
        mapping_ = nullptr;
    }
}
void NXPin::OpenMap()
{
    mapping_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, NXCAM_MAP_NAME);
    if (!mapping_)
        return;
    view_ = (uint8_t *)MapViewOfFile(mapping_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, NXCAM_MAP_BYTES);
    if (!view_)
    {
        CloseHandle(mapping_);
        mapping_ = nullptr;
        return;
    }
    auto *h = (NxCamHeader *)view_;
    if (h->magic != NXCAM_MAGIC || h->version != NXCAM_VERSION || h->width != NXCAM_W || h->height != NXCAM_H)
    {
        CloseMap();
        return;
    }
    InterlockedIncrement((volatile LONG *)&h->readers); // tells the app that a program is reading the camera
    counted_ = true;
}
// A dark picture with three lines of text, drawn once with GDI, so a camera program that opens the camera before a
// stream is running shows what is going on instead of an unexplained dark picture.
void NXPin::BuildWaiting()
{
    waiting_ = new (std::nothrow) uint8_t[NXCAM_SLOT_BYTES];
    if (!waiting_)
        return;
    uint32_t *px = (uint32_t *)waiting_;
    for (size_t i = 0; i < NXCAM_SLOT_BYTES / 4; ++i)
        px[i] = 0xFF16161F;
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = NXCAM_W;
    bi.bmiHeader.biHeight = -(LONG)NXCAM_H; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP bmp = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (bmp && bits)
    {
        HGDIOBJ old = SelectObject(dc, bmp);
        memcpy(bits, waiting_, NXCAM_SLOT_BYTES);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(226, 234, 255));
        HFONT big = CreateFontW(-60, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HFONT small = CreateFontW(-34, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                  CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        RECT r1 = {0, 250, (LONG)NXCAM_W, 340}, r2 = {0, 360, (LONG)NXCAM_W, 420}, r3 = {0, 420, (LONG)NXCAM_W, 480};
        HGDIOBJ prev = SelectObject(dc, big);
        DrawTextW(dc, L"NX Remote Play Camera", -1, &r1, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        SelectObject(dc, small);
        SetTextColor(dc, RGB(160, 176, 214));
        DrawTextW(dc, L"The camera works. Waiting for a stream in the desktop app.", -1, &r2,
                  DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        DrawTextW(dc, L"Connect the Switch and turn on \"Virtual camera\" in the app's Settings.", -1, &r3,
                  DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        SelectObject(dc, prev);
        GdiFlush();
        memcpy(waiting_, bits, NXCAM_SLOT_BYTES);
        SelectObject(dc, old);
        DeleteObject(big);
        DeleteObject(small);
    }
    if (bmp)
        DeleteObject(bmp);
    if (dc)
        DeleteDC(dc);
}
// Copies the newest picture (flipped when the connection is a bottom-up DIB) or the "waiting" picture.
void NXPin::Fill(uint8_t *dst)
{
    bool topDown = topDown_;
    const uint8_t *src = nullptr;
    if (!view_ && (mapRetry_++ % 20) == 0)
        OpenMap(); // the app may not be running yet: look again about every third of a second
    if (view_)
    {
        auto *h = (NxCamHeader *)view_;
        h->readerTickMs = GetTickCount64(); // tells the app that a program is reading the camera right now
        if (GetTickCount64() - h->producerTickMs < 2000)
        {
            MemoryBarrier();
            uint32_t slot = h->latestSlot % NXCAM_SLOTS;
            src = view_ + NXCAM_HEADER_BLOCK + (size_t)slot * NXCAM_SLOT_BYTES;
        }
        // A stale picture time only means no stream is running right now: keep the mapping (and the reader time stamp
        // the app watches). A producer that restarts opens the same section, because this handle keeps it alive.
    }
    if (!src)
    {
        if (!waiting_)
            BuildWaiting();
        src = waiting_;
    }
    if (!src)
    {
        memset(dst, 0x16, NXCAM_SLOT_BYTES);
        return;
    }
    if (topDown)
        memcpy(dst, src, NXCAM_SLOT_BYTES);
    else
        for (uint32_t y = 0; y < NXCAM_H; ++y)
            memcpy(dst + (size_t)y * NXCAM_STRIDE, src + (size_t)(NXCAM_H - 1 - y) * NXCAM_STRIDE, NXCAM_STRIDE);
}
void NXPin::Stream()
{
    LARGE_INTEGER freq, t0;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    const int fps = kFps[fpsIndex_];
    const LONGLONG interval = 10000000LL / fps; // 100 ns units
    LONGLONG frame = 0;
    bool first = true;
    timeBeginPeriod(1);
    for (;;)
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        LONGLONG elapsed100 = (now.QuadPart - t0.QuadPart) * 10000000LL / freq.QuadPart;
        LONGLONG due = frame * interval;
        if (elapsed100 + interval < due)
            frame = elapsed100 / interval; // never happens forwards
        if (elapsed100 > due + 3 * interval)
        {
            frame = elapsed100 / interval;
            due = frame * interval;
        } // far behind: skip ahead
        LONGLONG waitMs = (due - elapsed100) / 10000;
        if (waitMs > 0 && WaitForSingleObject(stop_, (DWORD)waitMs) == WAIT_OBJECT_0)
            break;
        if (WaitForSingleObject(stop_, 0) == WAIT_OBJECT_0)
            break;

        IMediaSample *sample = nullptr;
        if (FAILED(alloc_->GetBuffer(&sample, nullptr, nullptr, 0)) || !sample)
            break;
        BYTE *data = nullptr;
        sample->GetPointer(&data);
        if (!data || sample->GetSize() < (long)NXCAM_SLOT_BYTES)
        {
            sample->Release();
            break;
        }
        Fill(data);
        sample->SetActualDataLength((long)NXCAM_SLOT_BYTES);
        REFERENCE_TIME start, end;
        IReferenceClock *clock = filter_ ? ((NXFilter *)filter_)->ClockAddRef() : nullptr;
        if (clock)
        {
            REFERENCE_TIME gt = 0;
            clock->GetTime(&gt);
            start = gt - (REFERENCE_TIME)streamStart_;
            clock->Release();
        }
        else
            start = due;
        end = start + interval;
        sample->SetTime(&start, &end);
        sample->SetSyncPoint(TRUE);
        sample->SetDiscontinuity(first);
        first = false;
        HRESULT hr = mem_->Receive(sample);
        sample->Release();
        if (hr != S_OK)
            break; // S_FALSE = the receiver is flushing or stopped
        ++frame;
    }
    timeEndPeriod(1);
}

// ---------------------------------------------------------------------------------------------------------------- COM
// plumbing
class NXFactory : public IClassFactory
{
    virtual ~NXFactory()
    {
    }
    LONG ref_ = 1;

  public:
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_IClassFactory)
        {
            *ppv = static_cast<IClassFactory *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override
    {
        return InterlockedIncrement(&ref_);
    }
    STDMETHODIMP_(ULONG) Release() override
    {
        LONG r = InterlockedDecrement(&ref_);
        if (!r)
            delete this;
        return r;
    }
    STDMETHODIMP CreateInstance(IUnknown *outer, REFIID riid, void **ppv) override
    {
        if (!ppv)
            return E_POINTER;
        *ppv = nullptr;
        if (outer)
            return CLASS_E_NOAGGREGATION;
        auto *f = new (std::nothrow) NXFilter();
        if (!f)
            return E_OUTOFMEMORY;
        HRESULT hr = f->QueryInterface(riid, ppv);
        f->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL lock) override
    {
        if (lock)
            InterlockedIncrement(&g_objects);
        else
            InterlockedDecrement(&g_objects);
        return S_OK;
    }
};

extern "C" BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = inst;
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}
extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID riid, void **ppv)
{
    if (!ppv)
        return E_POINTER;
    *ppv = nullptr;
    if (clsid != CLSID_NXRemotePlayCam)
        return CLASS_E_CLASSNOTAVAILABLE;
    auto *f = new (std::nothrow) NXFactory();
    if (!f)
        return E_OUTOFMEMORY;
    HRESULT hr = f->QueryInterface(riid, ppv);
    f->Release();
    return hr;
}
extern "C" HRESULT __stdcall DllCanUnloadNow()
{
    return g_objects == 0 ? S_OK : S_FALSE;
}

// Registration is per user by default (regsvr32 NXRemotePlayCam.dll): every key goes under
// HKEY_CURRENT_USER\Software\Classes (HKEY_CLASSES_ROOT is redirected there for this process while we register), so no
// administrator rights are needed and programs running as this user see the camera. Programs started "as administrator"
// (OBS is often set that way) ignore per-user COM registrations, so there is also an all-users registration under
// HKEY_LOCAL_MACHINE that needs an elevated process:
//   regsvr32 /n /i:machine NXRemotePlayCam.dll        (remove: regsvr32 /u /n /i:machine NXRemotePlayCam.dll)
static HKEY RedirectClasses(HKEY hive)
{
    HKEY classes = nullptr;
    if (RegCreateKeyExW(hive, L"Software\\Classes", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &classes, nullptr) !=
        ERROR_SUCCESS)
        return nullptr;
    if (RegOverridePredefKey(HKEY_CLASSES_ROOT, classes) != ERROR_SUCCESS)
    {
        RegCloseKey(classes);
        return nullptr;
    }
    return classes;
}
static void EndRedirect(HKEY classes)
{
    RegOverridePredefKey(HKEY_CLASSES_ROOT, nullptr);
    if (classes)
        RegCloseKey(classes);
}
static LONG SetString(HKEY key, const wchar_t *name, const wchar_t *value)
{
    return RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)value, (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
}
static HRESULT RegisterImpl(HKEY hive)
{
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(g_module, path, MAX_PATH))
        return SELFREG_E_CLASS;
    wchar_t *idText = nullptr;
    if (FAILED(StringFromCLSID(CLSID_NXRemotePlayCam, &idText)))
        return SELFREG_E_CLASS;
    HKEY classes = RedirectClasses(hive);
    if (!classes)
    {
        CoTaskMemFree(idText);
        return SELFREG_E_CLASS;
    }
    HRESULT result = S_OK;
    wchar_t keyPath[200];
    HKEY key = nullptr;
    wsprintfW(keyPath, L"CLSID\\%s", idText);
    if (RegCreateKeyExW(HKEY_CLASSES_ROOT, keyPath, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &key, nullptr) ==
        ERROR_SUCCESS)
    {
        SetString(key, nullptr, kFilterName);
        RegCloseKey(key);
    }
    else
        result = SELFREG_E_CLASS;
    wsprintfW(keyPath, L"CLSID\\%s\\InprocServer32", idText);
    if (SUCCEEDED(result) && RegCreateKeyExW(HKEY_CLASSES_ROOT, keyPath, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &key,
                                             nullptr) == ERROR_SUCCESS)
    {
        SetString(key, nullptr, path);
        SetString(key, L"ThreadingModel", L"Both");
        RegCloseKey(key);
    }
    else
        result = SELFREG_E_CLASS;
    if (SUCCEEDED(result))
    {
        bool initialised = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        IFilterMapper2 *mapper = nullptr;
        HRESULT hr =
            CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER, IID_IFilterMapper2, (void **)&mapper);
        if (SUCCEEDED(hr))
        {
            REGPINTYPES types = {&MEDIATYPE_Video, &MEDIASUBTYPE_RGB32};
            REGFILTERPINS pins = {};
            pins.strName = (LPWSTR)L"Capture";
            pins.bRendered = FALSE;
            pins.bOutput = TRUE;
            pins.bZero = FALSE;
            pins.bMany = FALSE;
            pins.clsConnectsToFilter = nullptr;
            pins.strConnectsToPin = nullptr;
            pins.nMediaTypes = 1;
            pins.lpMediaType = &types;
            REGFILTER2 filter = {};
            filter.dwVersion = 1;
            filter.dwMerit = MERIT_DO_NOT_USE;
            filter.cPins = 1;
            filter.rgPins = &pins;
            hr = mapper->RegisterFilter(CLSID_NXRemotePlayCam, kFilterName, nullptr, &CLSID_VideoInputDeviceCategory,
                                        kFilterName, &filter);
            mapper->Release();
        }
        if (FAILED(hr))
            result = SELFREG_E_CLASS;
        if (initialised)
            CoUninitialize();
    }
    EndRedirect(classes);
    CoTaskMemFree(idText);
    return result;
}
static HRESULT UnregisterImpl(HKEY hive)
{
    wchar_t *idText = nullptr;
    if (FAILED(StringFromCLSID(CLSID_NXRemotePlayCam, &idText)))
        return SELFREG_E_CLASS;
    HKEY classes = RedirectClasses(hive);
    if (!classes)
    {
        CoTaskMemFree(idText);
        return SELFREG_E_CLASS;
    }
    bool initialised = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    IFilterMapper2 *mapper = nullptr;
    if (SUCCEEDED(
            CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER, IID_IFilterMapper2, (void **)&mapper)))
    {
        mapper->UnregisterFilter(&CLSID_VideoInputDeviceCategory, kFilterName, CLSID_NXRemotePlayCam);
        mapper->Release();
    }
    if (initialised)
        CoUninitialize();
    wchar_t keyPath[200];
    wsprintfW(keyPath, L"CLSID\\%s\\InprocServer32", idText);
    RegDeleteKeyW(HKEY_CLASSES_ROOT, keyPath);
    wsprintfW(keyPath, L"CLSID\\%s", idText);
    RegDeleteKeyW(HKEY_CLASSES_ROOT, keyPath);
    EndRedirect(classes);
    CoTaskMemFree(idText);
    return S_OK;
}
extern "C" HRESULT __stdcall DllRegisterServer()
{
    return RegisterImpl(HKEY_CURRENT_USER);
}
extern "C" HRESULT __stdcall DllUnregisterServer()
{
    return UnregisterImpl(HKEY_CURRENT_USER);
}
// regsvr32 /n /i:<cmd>: "machine" = all users (HKEY_LOCAL_MACHINE, needs an elevated process), anything else = this
// user.
extern "C" HRESULT __stdcall DllInstall(BOOL install, PCWSTR cmd)
{
    HKEY hive = (cmd && _wcsicmp(cmd, L"machine") == 0) ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
    return install ? RegisterImpl(hive) : UnregisterImpl(hive);
}
