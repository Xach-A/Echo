#include "audio/WasapiStream.h"

#include <avrt.h>

#include <algorithm>
#include <cstring>

#include "core/Common.h"
#include "core/Log.h"

// Process-loopback activation types (audioclientactivationparams.h is not
// shipped by every SDK, so they are declared here per the Windows docs).
namespace {
enum EchoActivationType { kActivationDefault = 0, kActivationProcessLoopback = 1 };
enum EchoLoopbackMode { kIncludeTargetProcessTree = 0, kExcludeTargetProcessTree = 1 };
struct EchoProcessLoopbackParams {
    DWORD TargetProcessId;
    DWORD ProcessLoopbackMode;
};
struct EchoActivationParams {
    DWORD ActivationType;
    EchoProcessLoopbackParams ProcessLoopbackParams;
};
const wchar_t* const kProcessLoopbackDevice = L"VAD\\Process_Loopback";
} // namespace

namespace echo::win {

namespace {

constexpr REFERENCE_TIME kHnsPerSec = 10000000;

int64_t nowTicksMs() { return static_cast<int64_t>(GetTickCount64()); }

void registerMmcss(HANDLE* outTask) {
    DWORD idx = 0;
    *outTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &idx);
    if (*outTask) AvSetMmThreadPriority(*outTask, AVRT_PRIORITY_HIGH);
    else SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
}

class ActivationHandler final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
public:
    ActivationHandler() { done = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
    ~ActivationHandler() { CloseHandle(done); }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --ref_;
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        } else if (riid == __uuidof(IAgileObject)) {
            *ppv = static_cast<IAgileObject*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
        HRESULT hrAct = E_FAIL;
        ComPtr<IUnknown> unk;
        const HRESULT hrGet = op->GetActivateResult(&hrAct, &unk);
        result = FAILED(hrGet) ? hrGet : hrAct;
        if (SUCCEEDED(result) && unk) result = unk.As(&client);
        SetEvent(done);
        return S_OK;
    }
    HANDLE done;
    HRESULT result = E_PENDING;
    ComPtr<IAudioClient> client;

private:
    std::atomic<ULONG> ref_{1};
};

HRESULT activateProcessLoopback(DWORD pid, bool exclude, ComPtr<IAudioClient>& out) {
    EchoActivationParams params{};
    params.ActivationType = kActivationProcessLoopback;
    params.ProcessLoopbackParams.TargetProcessId = pid;
    params.ProcessLoopbackParams.ProcessLoopbackMode = exclude ? kExcludeTargetProcessTree
                                                               : kIncludeTargetProcessTree;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof(params);
    pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    auto* handler = new ActivationHandler();
    ComPtr<IActivateAudioInterfaceAsyncOperation> op;
    HRESULT hr = ActivateAudioInterfaceAsync(kProcessLoopbackDevice, __uuidof(IAudioClient), &pv, handler, &op);
    if (SUCCEEDED(hr)) {
        if (WaitForSingleObject(handler->done, 5000) != WAIT_OBJECT_0) hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        else hr = handler->result;
        if (SUCCEEDED(hr)) out = handler->client;
    }
    handler->Release();
    return hr;
}

// Initialises an exclusive-mode event-driven stream, trying common formats
// and re-aligning the period when the driver requires it.
HRESULT initExclusive(IMMDevice* dev, ComPtr<IAudioClient>& client, int preferredRate, int channels,
                      DeviceFormat& fmtOut, REFERENCE_TIME& periodOut) {
    const SampleKind kinds[] = {SampleKind::Float32, SampleKind::Int32, SampleKind::Int24, SampleKind::Int16};
    WAVEFORMATEXTENSIBLE chosen{};
    bool found = false;
    for (SampleKind k : kinds) {
        WAVEFORMATEXTENSIBLE wf = makeFormat(preferredRate, channels, k);
        if (client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &wf.Format, nullptr) == S_OK) {
            chosen = wf;
            found = true;
            break;
        }
        if (k == SampleKind::Int32) {
            wf.Samples.wValidBitsPerSample = 24; // 24-in-32 is common for USB devices
            if (client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &wf.Format, nullptr) == S_OK) {
                chosen = wf;
                found = true;
                break;
            }
        }
    }
    if (!found) return AUDCLNT_E_UNSUPPORTED_FORMAT;

    REFERENCE_TIME defPeriod = 0, minPeriod = 0;
    client->GetDevicePeriod(&defPeriod, &minPeriod);
    REFERENCE_TIME period = std::max(minPeriod, static_cast<REFERENCE_TIME>(30000)); // >= 3 ms
    HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period,
                                    period, &chosen.Format, nullptr);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        UINT32 frames = 0;
        client->GetBufferSize(&frames);
        period = static_cast<REFERENCE_TIME>(
            static_cast<double>(kHnsPerSec) * frames / chosen.Format.nSamplesPerSec + 0.5);
        client.Reset();
        hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()));
        if (FAILED(hr)) return hr;
        hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period,
                                &chosen.Format, nullptr);
    }
    if (SUCCEEDED(hr)) {
        fmtOut = describeFormat(&chosen.Format);
        periodOut = period;
    }
    return hr;
}

HRESULT openDevice(const std::wstring& id, ComPtr<IMMDevice>& dev) {
    ComPtr<IMMDeviceEnumerator> en;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    if (FAILED(hr)) return hr;
    return en->GetDevice(id.c_str(), &dev);
}

} // namespace

bool processLoopbackSupported() {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    static const bool supported = [] {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        auto fn = ntdll ? reinterpret_cast<RtlGetVersionFn>(
                              reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")))
                        : nullptr;
        RTL_OSVERSIONINFOW vi{};
        vi.dwOSVersionInfoSize = sizeof(vi);
        if (!fn || fn(&vi) != 0) return false;
        return vi.dwMajorVersion > 10 || (vi.dwMajorVersion == 10 && vi.dwBuildNumber >= 19041);
    }();
    return supported;
}

// ===========================================================================
// Capture
// ===========================================================================

WasapiCapture::WasapiCapture(const StreamOptions& opt) : opt_(opt) {
    audioEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    readyEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

WasapiCapture::~WasapiCapture() {
    SetEvent(stopEvent_);
    if (thread_.joinable()) thread_.join();
    CloseHandle(audioEvent_);
    CloseHandle(stopEvent_);
    CloseHandle(readyEvent_);
}

std::shared_ptr<WasapiCapture> WasapiCapture::openAsync(const StreamOptions& opt) {
    std::shared_ptr<WasapiCapture> s(new WasapiCapture(opt));
    s->thread_ = std::thread(&WasapiCapture::threadMain, s.get());
    return s;
}

std::shared_ptr<WasapiCapture> WasapiCapture::open(const StreamOptions& opt, std::string* error) {
    std::shared_ptr<WasapiCapture> s = openAsync(opt);
    if (WaitForSingleObject(s->readyEvent_, 6000) != WAIT_OBJECT_0) {
        if (error) *error = "Timed out opening capture stream";
        return nullptr; // destructor stops the thread
    }
    if (s->status() != EndpointStatus::Running) {
        if (error) *error = s->lastError();
        return nullptr;
    }
    return s;
}

std::string WasapiCapture::lastError() const {
    std::lock_guard<std::mutex> lock(errMutex_);
    return error_;
}

double WasapiCapture::msSinceLastPacket() const {
    return static_cast<double>(nowTicksMs() - lastPacketTick_.load());
}

HRESULT WasapiCapture::initialize() {
    HRESULT hr;
    REFERENCE_TIME period = 100000; // 10 ms default
    if (opt_.captureMode == CaptureMode::ProcessLoopback) {
        if (!processLoopbackSupported()) return E_NOTIMPL;
        hr = activateProcessLoopback(opt_.processId, !opt_.includeProcessTree, client_);
        if (FAILED(hr)) return hr;
        WAVEFORMATEXTENSIBLE wf = makeFormat(opt_.preferredSampleRate, 2, SampleKind::Float32);
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                     AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                 kHnsPerSec / 5, 0, &wf.Format, nullptr);
        if (FAILED(hr)) {
            // Older builds only accept plain PCM here.
            client_.Reset();
            hr = activateProcessLoopback(opt_.processId, !opt_.includeProcessTree, client_);
            if (FAILED(hr)) return hr;
            WAVEFORMATEX pcm{};
            pcm.wFormatTag = WAVE_FORMAT_PCM;
            pcm.nChannels = 2;
            pcm.nSamplesPerSec = 48000;
            pcm.wBitsPerSample = 16;
            pcm.nBlockAlign = 4;
            pcm.nAvgBytesPerSec = 48000 * 4;
            hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                     AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                         AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                                     kHnsPerSec / 5, 0, &pcm, nullptr);
            if (FAILED(hr)) return hr;
            format_ = describeFormat(&pcm);
        } else {
            format_ = describeFormat(&wf.Format);
        }
    } else {
        ComPtr<IMMDevice> dev;
        hr = openDevice(opt_.deviceId, dev);
        if (FAILED(hr)) return hr;
        hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client_.GetAddressOf()));
        if (FAILED(hr)) return hr;
        WAVEFORMATEX* mix = nullptr;
        hr = client_->GetMixFormat(&mix);
        if (FAILED(hr)) return hr;
        const DeviceFormat mixFmt = describeFormat(mix);
        const bool loop = opt_.captureMode == CaptureMode::Loopback;
        if (opt_.exclusive && !loop) {
            hr = initExclusive(dev.Get(), client_, opt_.preferredSampleRate, mixFmt.channels, format_, period);
            if (FAILED(hr)) {
                ECHO_LOG_WARN("Exclusive capture failed on %s (%s); falling back to shared mode",
                              opt_.label.c_str(), hrToString(hr).c_str());
                client_.Reset();
                hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client_.GetAddressOf()));
                if (FAILED(hr)) {
                    CoTaskMemFree(mix);
                    return hr;
                }
                opt_.exclusive = false;
            }
        }
        if (!opt_.exclusive || loop) {
            DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
            if (loop) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
            hr = E_FAIL;
            if (opt_.lowLatencyShared && !loop) {
                ComPtr<IAudioClient3> c3;
                if (SUCCEEDED(client_.As(&c3))) {
                    UINT32 def = 0, fund = 0, mn = 0, mx = 0;
                    if (SUCCEEDED(c3->GetSharedModeEnginePeriod(mix, &def, &fund, &mn, &mx)))
                        hr = c3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, mn, mix, nullptr);
                }
            }
            if (FAILED(hr)) hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kHnsPerSec / 10, 0, mix, nullptr);
            format_ = mixFmt;
            REFERENCE_TIME defPeriod = 0, minPeriod = 0;
            if (SUCCEEDED(client_->GetDevicePeriod(&defPeriod, &minPeriod)) && defPeriod > 0) period = defPeriod;
        }
        CoTaskMemFree(mix);
        if (FAILED(hr)) return hr;
    }
    if (format_.kind == SampleKind::Unknown || format_.channels <= 0) return AUDCLNT_E_UNSUPPORTED_FORMAT;

    hr = client_->SetEventHandle(audioEvent_);
    if (FAILED(hr)) return hr;
    hr = client_->GetService(IID_PPV_ARGS(&capture_));
    if (FAILED(hr)) return hr;
    client_->GetBufferSize(&bufferFrames_);
    periodFrames_ = std::max(16, static_cast<int>(static_cast<double>(period) * format_.sampleRate / kHnsPerSec));
    REFERENCE_TIME lat = 0;
    if (SUCCEEDED(client_->GetStreamLatency(&lat))) latencySec_ = static_cast<double>(lat) / kHnsPerSec;
    latencySec_ += static_cast<double>(periodFrames_) / format_.sampleRate;

    ring_.allocate(static_cast<size_t>(format_.sampleRate * opt_.ringSeconds) * format_.channels);
    scratch_.assign(static_cast<size_t>(std::max<UINT32>(bufferFrames_, 48000)) * format_.channels, 0.0f);
    return client_->Start();
}

void WasapiCapture::threadMain() {
    ComScope com(COINIT_MULTITHREADED);
    enableFlushDenormals();
    HRESULT hr = initialize();
    if (FAILED(hr)) {
        {
            std::lock_guard<std::mutex> lock(errMutex_);
            error_ = hrToString(hr);
        }
        status_.store(static_cast<int>(EndpointStatus::Error));
        ECHO_LOG_WARN("Capture open failed (%s): %s", opt_.label.c_str(), error_.c_str());
        capture_.Reset();
        client_.Reset();
        SetEvent(readyEvent_);
        return;
    }
    HANDLE task = nullptr;
    registerMmcss(&task);
    lastPacketTick_.store(nowTicksMs());
    status_.store(static_cast<int>(EndpointStatus::Running));
    SetEvent(readyEvent_);
    ECHO_LOG_INFO("Capture started: %s [%s, period %d frames, buffer %u]", opt_.label.c_str(),
                  formatToString(format_).c_str(), periodFrames_, bufferFrames_);

    const bool sparse = isSparse();
    const DWORD timeoutMs = sparse ? 10 : 2000;
    const int ch = format_.channels;
    bool firstPacket = true;
    HANDLE hs[2] = {stopEvent_, audioEvent_};
    for (;;) {
        const DWORD r = WaitForMultipleObjects(2, hs, FALSE, timeoutMs);
        if (r == WAIT_OBJECT_0) break;
        stats.wakeups.fetch_add(1, std::memory_order_relaxed);
        UINT32 packet = 0;
        hr = capture_->GetNextPacketSize(&packet);
        while (SUCCEEDED(hr) && packet > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            hr = capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;
            if (hr == AUDCLNT_S_BUFFER_EMPTY || frames == 0) {
                capture_->ReleaseBuffer(0);
                break;
            }
            const size_t samples = static_cast<size_t>(frames) * ch;
            if (samples <= scratch_.size()) {
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    std::fill_n(scratch_.data(), samples, 0.0f);
                } else {
                    deviceToFloat(data, scratch_.data(), samples, format_);
                    if (!sawNonSilence_.load(std::memory_order_relaxed)) {
                        for (size_t i = 0; i < samples; ++i)
                            if (scratch_[i] != 0.0f) {
                                sawNonSilence_.store(true, std::memory_order_relaxed);
                                break;
                            }
                    }
                }
                const size_t written = ring_.write(scratch_.data(), samples);
                if (written < samples) stats.overruns.fetch_add(1, std::memory_order_relaxed);
                flowTotal_ += written / static_cast<size_t>(ch);
                flow_.publish(flowTotal_, static_cast<uint32_t>(written / static_cast<size_t>(ch)), monotonicSeconds());
            }
            if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) && !firstPacket)
                stats.discontinuities.fetch_add(1, std::memory_order_relaxed);
            firstPacket = false;
            stats.frames.fetch_add(frames, std::memory_order_relaxed);
            capture_->ReleaseBuffer(frames);
            lastPacketTick_.store(nowTicksMs(), std::memory_order_relaxed);
            hr = capture_->GetNextPacketSize(&packet);
        }
        if (FAILED(hr)) {
            const bool lost = hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING;
            {
                std::lock_guard<std::mutex> lock(errMutex_);
                error_ = hrToString(hr);
            }
            status_.store(static_cast<int>(lost ? EndpointStatus::Lost : EndpointStatus::Error));
            ECHO_LOG_WARN("Capture stream ended (%s): %s", opt_.label.c_str(), error_.c_str());
            break;
        }
    }
    if (client_) client_->Stop();
    capture_.Reset();
    client_.Reset();
    if (task) AvRevertMmThreadCharacteristics(task);
    int expected = static_cast<int>(EndpointStatus::Running);
    status_.compare_exchange_strong(expected, static_cast<int>(EndpointStatus::Stopped));
}

// ===========================================================================
// Render
// ===========================================================================

WasapiRender::WasapiRender(const StreamOptions& opt) : opt_(opt) {
    audioEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    readyEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

WasapiRender::~WasapiRender() {
    wake_.store(nullptr);
    SetEvent(stopEvent_);
    if (thread_.joinable()) thread_.join();
    CloseHandle(audioEvent_);
    CloseHandle(stopEvent_);
    CloseHandle(readyEvent_);
}

std::shared_ptr<WasapiRender> WasapiRender::openAsync(const StreamOptions& opt) {
    std::shared_ptr<WasapiRender> s(new WasapiRender(opt));
    s->thread_ = std::thread(&WasapiRender::threadMain, s.get());
    return s;
}

std::shared_ptr<WasapiRender> WasapiRender::open(const StreamOptions& opt, std::string* error) {
    std::shared_ptr<WasapiRender> s = openAsync(opt);
    if (WaitForSingleObject(s->readyEvent_, 6000) != WAIT_OBJECT_0) {
        if (error) *error = "Timed out opening render stream";
        return nullptr;
    }
    if (s->status() != EndpointStatus::Running) {
        if (error) *error = s->lastError();
        return nullptr;
    }
    return s;
}

std::string WasapiRender::lastError() const {
    std::lock_guard<std::mutex> lock(errMutex_);
    return error_;
}

void WasapiRender::setWakeTarget(WakeTarget* target, int thresholdFrames) {
    wakeThreshold_.store(thresholdFrames);
    wake_.store(target);
    if (target) target->wake();
}

HRESULT WasapiRender::initialize() {
    ComPtr<IMMDevice> dev;
    HRESULT hr = openDevice(opt_.deviceId, dev);
    if (FAILED(hr)) return hr;
    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client_.GetAddressOf()));
    if (FAILED(hr)) return hr;
    WAVEFORMATEX* mix = nullptr;
    hr = client_->GetMixFormat(&mix);
    if (FAILED(hr)) return hr;
    const DeviceFormat mixFmt = describeFormat(mix);
    REFERENCE_TIME period = 100000;
    bool exclusive = false;
    if (opt_.exclusive) {
        hr = initExclusive(dev.Get(), client_, opt_.preferredSampleRate, mixFmt.channels, format_, period);
        if (SUCCEEDED(hr)) {
            exclusive = true;
        } else {
            ECHO_LOG_WARN("Exclusive render failed on %s (%s); falling back to shared mode", opt_.label.c_str(),
                          hrToString(hr).c_str());
            client_.Reset();
            hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client_.GetAddressOf()));
            if (FAILED(hr)) {
                CoTaskMemFree(mix);
                return hr;
            }
            opt_.exclusive = false;
        }
    }
    if (!exclusive) {
        hr = E_FAIL;
        if (opt_.lowLatencyShared) {
            ComPtr<IAudioClient3> c3;
            if (SUCCEEDED(client_.As(&c3))) {
                UINT32 def = 0, fund = 0, mn = 0, mx = 0;
                if (SUCCEEDED(c3->GetSharedModeEnginePeriod(mix, &def, &fund, &mn, &mx))) {
                    hr = c3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, mn, mix, nullptr);
                    if (SUCCEEDED(hr)) period = static_cast<REFERENCE_TIME>(static_cast<double>(mn) * kHnsPerSec / mixFmt.sampleRate);
                }
            }
        }
        if (FAILED(hr)) {
            hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, kHnsPerSec * 3 / 100, 0,
                                     mix, nullptr);
            REFERENCE_TIME defPeriod = 0, minPeriod = 0;
            if (SUCCEEDED(client_->GetDevicePeriod(&defPeriod, &minPeriod)) && defPeriod > 0) period = defPeriod;
        }
        format_ = mixFmt;
    }
    CoTaskMemFree(mix);
    if (FAILED(hr)) return hr;
    if (format_.kind == SampleKind::Unknown || format_.channels <= 0) return AUDCLNT_E_UNSUPPORTED_FORMAT;

    hr = client_->SetEventHandle(audioEvent_);
    if (FAILED(hr)) return hr;
    hr = client_->GetService(IID_PPV_ARGS(&render_));
    if (FAILED(hr)) return hr;
    client_->GetBufferSize(&bufferFrames_);
    periodFrames_ = std::max(16, static_cast<int>(static_cast<double>(period) * format_.sampleRate / kHnsPerSec));
    padTarget_ = exclusive ? static_cast<int>(bufferFrames_)
                           : std::min(static_cast<int>(bufferFrames_), 2 * periodFrames_);
    REFERENCE_TIME lat = 0;
    if (SUCCEEDED(client_->GetStreamLatency(&lat))) latencySec_ = static_cast<double>(lat) / kHnsPerSec;
    latencySec_ += static_cast<double>(padTarget_) / format_.sampleRate;

    ring_.allocate(static_cast<size_t>(format_.sampleRate * opt_.ringSeconds) * format_.channels);
    scratch_.assign(static_cast<size_t>(bufferFrames_) * format_.channels, 0.0f);

    // Pre-roll one period of silence so the device starts cleanly.
    BYTE* data = nullptr;
    const UINT32 pre = exclusive ? bufferFrames_ : static_cast<UINT32>(std::min<int>(periodFrames_, bufferFrames_));
    if (SUCCEEDED(render_->GetBuffer(pre, &data))) render_->ReleaseBuffer(pre, AUDCLNT_BUFFERFLAGS_SILENT);
    opt_.exclusive = exclusive;
    return client_->Start();
}

void WasapiRender::threadMain() {
    ComScope com(COINIT_MULTITHREADED);
    enableFlushDenormals();
    HRESULT hr = initialize();
    if (FAILED(hr)) {
        {
            std::lock_guard<std::mutex> lock(errMutex_);
            error_ = hrToString(hr);
        }
        status_.store(static_cast<int>(EndpointStatus::Error));
        ECHO_LOG_WARN("Render open failed (%s): %s", opt_.label.c_str(), error_.c_str());
        render_.Reset();
        client_.Reset();
        SetEvent(readyEvent_);
        return;
    }
    HANDLE task = nullptr;
    registerMmcss(&task);
    status_.store(static_cast<int>(EndpointStatus::Running));
    SetEvent(readyEvent_);
    ECHO_LOG_INFO("Render started: %s [%s, period %d frames, buffer %u%s]", opt_.label.c_str(),
                  formatToString(format_).c_str(), periodFrames_, bufferFrames_, opt_.exclusive ? ", exclusive" : "");

    const int ch = format_.channels;
    bool primed = false;
    HANDLE hs[2] = {stopEvent_, audioEvent_};
    for (;;) {
        const DWORD r = WaitForMultipleObjects(2, hs, FALSE, 2000);
        if (r == WAIT_OBJECT_0) break;
        stats.wakeups.fetch_add(1, std::memory_order_relaxed);
        UINT32 padding = 0;
        if (!opt_.exclusive) {
            hr = client_->GetCurrentPadding(&padding);
            if (FAILED(hr)) break;
        }
        if (r == WAIT_TIMEOUT) continue; // device paused (e.g. sleeping); keep waiting

        const int avail = static_cast<int>(bufferFrames_) - static_cast<int>(padding);
        int want = opt_.exclusive ? static_cast<int>(bufferFrames_)
                                  : std::clamp(padTarget_ - static_cast<int>(padding), 0, avail);
        const int ringFrames = static_cast<int>(ring_.readAvailable() / static_cast<size_t>(ch));
        int n = std::min(want, ringFrames);
        const int minNeeded = opt_.exclusive ? static_cast<int>(bufferFrames_)
                                             : std::max(0, periodFrames_ - static_cast<int>(padding));
        int silence = 0;
        if (n < minNeeded) {
            silence = minNeeded - n;
            if (primed) stats.underruns.fetch_add(1, std::memory_order_relaxed);
        }
        const int total = n + silence;
        if (total > 0) {
            BYTE* data = nullptr;
            hr = render_->GetBuffer(static_cast<UINT32>(total), &data);
            if (FAILED(hr)) break;
            if (n > 0) {
                ring_.read(scratch_.data(), static_cast<size_t>(n) * ch);
                floatToDevice(scratch_.data(), data, static_cast<size_t>(n) * ch, format_);
                primed = true;
                flowTotal_ += static_cast<uint64_t>(n);
                flow_.publish(flowTotal_, static_cast<uint32_t>(n), monotonicSeconds());
            }
            if (silence > 0)
                std::memset(data + static_cast<size_t>(n) * format_.blockAlign, 0,
                            static_cast<size_t>(silence) * format_.blockAlign);
            hr = render_->ReleaseBuffer(static_cast<UINT32>(total), 0);
            if (FAILED(hr)) break;
            stats.frames.fetch_add(static_cast<uint64_t>(total), std::memory_order_relaxed);
        }
        if (WakeTarget* w = wake_.load(std::memory_order_acquire)) {
            if (static_cast<int>(ring_.readAvailable() / static_cast<size_t>(ch)) <
                wakeThreshold_.load(std::memory_order_relaxed))
                w->wake();
        }
    }
    if (FAILED(hr)) {
        const bool lost = hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING;
        {
            std::lock_guard<std::mutex> lock(errMutex_);
            error_ = hrToString(hr);
        }
        status_.store(static_cast<int>(lost ? EndpointStatus::Lost : EndpointStatus::Error));
        ECHO_LOG_WARN("Render stream ended (%s): %s", opt_.label.c_str(), error_.c_str());
    }
    if (client_) client_->Stop();
    render_.Reset();
    client_.Reset();
    if (task) AvRevertMmThreadCharacteristics(task);
    int expected = static_cast<int>(EndpointStatus::Running);
    status_.compare_exchange_strong(expected, static_cast<int>(EndpointStatus::Stopped));
}

} // namespace echo::win
