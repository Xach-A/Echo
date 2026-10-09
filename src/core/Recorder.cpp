#include "core/Recorder.h"

#include <chrono>
#include <ctime>
#include <filesystem>

#include "core/Log.h"

namespace echo {

namespace {

std::wstring widen(const std::string& s) {
    std::wstring w;
    for (unsigned char c : s) w.push_back(c < 0x80 ? static_cast<wchar_t>(c) : L'_');
    return w;
}

std::wstring sanitize(std::wstring s) {
    for (auto& c : s)
        if (c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' || c == L'"' || c == L'<' || c == L'>' ||
            c == L'|')
            c = L'_';
    return s;
}

} // namespace

Recorder::~Recorder() { stop(); }

bool Recorder::start(const std::vector<RecordingTarget>& targets, const std::wstring& folder, int engineRate,
                     int fileRate, int bitDepth, std::string* error) {
    stop();
    if (targets.empty()) {
        if (error) *error = "Nothing selected to record";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    if (ec) {
        if (error) *error = "Cannot create recording folder: " + ec.message();
        return false;
    }
    fileRate_ = fileRate > 0 ? fileRate : engineRate;
    ratio_ = static_cast<double>(engineRate) / fileRate_;

    std::time_t now = std::time(nullptr);
    std::tm tmv{};
    localtime_s(&tmv, &now);
    wchar_t stamp[32];
    std::wcsftime(stamp, 32, L"%Y-%m-%d_%H-%M-%S", &tmv);

    taps_.clear();
    writers_.clear();
    files_.clear();
    for (const auto& t : targets) {
        auto w = std::make_unique<Writer>();
        // ~4 seconds of stereo headroom between the engine and the disk.
        w->tap = std::make_shared<RecordTap>(static_cast<size_t>(engineRate) * 4);
        const std::wstring path = (std::filesystem::path(folder) /
                                   (std::wstring(L"Echo_") + stamp + L"_" + sanitize(widen(t.label)) + L".wav"))
                                      .wstring();
        if (!w->wav.open(path, fileRate_, 2, bitDepth)) {
            if (error) *error = "Cannot create file in the recording folder";
            taps_.clear();
            writers_.clear();
            return false;
        }
        if (fileRate_ != engineRate) {
            w->rs = std::make_unique<Resampler>();
            w->rs->prepare(2, ratio_, engineRate, ResamplerQuality::High);
        }
        files_.push_back(path);
        taps_.push_back(ActiveTap{t, w->tap});
        writers_.push_back(std::move(w));
    }
    frames_ = 0;
    bytes_ = 0;
    stopRequested_ = false;
    active_ = true;
    thread_ = std::thread(&Recorder::writerMain, this);
    ECHO_LOG_INFO("Recording started: %zu target(s), %d Hz, %d-bit", targets.size(), fileRate_, bitDepth);
    return true;
}

void Recorder::stop() {
    if (!active_.load()) return;
    stopRequested_ = true;
    if (thread_.joinable()) thread_.join();
    for (auto& w : writers_) w->wav.close();
    active_ = false;
    ECHO_LOG_INFO("Recording stopped (%.1f s)", static_cast<double>(frames_.load()) / fileRate_);
    taps_.clear();
    writers_.clear();
}

RecordingStatus Recorder::status() const {
    RecordingStatus s;
    s.active = active_.load();
    s.seconds = static_cast<double>(frames_.load()) / std::max(1, fileRate_);
    s.bytes = bytes_.load();
    for (const auto& t : taps_) s.droppedFrames += t.tap->droppedFrames.load();
    s.files = files_;
    std::lock_guard<std::mutex> lock(mutex_);
    s.error = error_;
    return s;
}

void Recorder::writerMain() {
    auto lastHeader = std::chrono::steady_clock::now();
    for (;;) {
        const bool stopping = stopRequested_.load();
        uint64_t maxFrames = 0;
        uint64_t totalBytes = 0;
        for (auto& w : writers_) {
            SpscRing<float>& ring = w->tap->ring;
            const size_t avail = ring.readAvailable() & ~static_cast<size_t>(1);
            if (avail > 0) {
                w->in.resize(avail);
                ring.read(w->in.data(), avail);
                const size_t frames = avail / 2;
                if (w->rs) {
                    size_t done = 0;
                    while (done < frames) {
                        const int chunk = static_cast<int>(std::min<size_t>(frames - done, 4096));
                        w->rs->push(w->in.data() + done * 2, chunk);
                        done += chunk;
                        const int outN = w->rs->outputAvailable(ratio_);
                        w->out.resize(static_cast<size_t>(outN) * 2);
                        const int got = w->rs->pull(w->out.data(), outN, ratio_);
                        if (!w->wav.write(w->out.data(), got)) {
                            std::lock_guard<std::mutex> lock(mutex_);
                            error_ = "Disk write failed (disk full?)";
                        }
                    }
                } else if (!w->wav.write(w->in.data(), frames)) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    error_ = "Disk write failed (disk full?)";
                }
            }
            maxFrames = std::max<uint64_t>(maxFrames, w->wav.framesWritten());
            totalBytes += w->wav.bytesWritten();
        }
        frames_ = maxFrames;
        bytes_ = totalBytes;
        const auto now = std::chrono::steady_clock::now();
        if (now - lastHeader > std::chrono::seconds(2)) {
            // Keep headers current so a crash still leaves playable files.
            for (auto& w : writers_) w->wav.updateHeader();
            lastHeader = now;
        }
        if (stopping) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

} // namespace echo
