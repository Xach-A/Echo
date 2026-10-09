#include "core/Engine.h"

#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <avrt.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace echo {

struct Engine::Impl : WakeTarget {
    HANDLE wakeEvent = nullptr;
    HANDLE stopEvent = nullptr;
    HANDLE timer = nullptr;
    LARGE_INTEGER qpcFreq{};
    void wake() override { SetEvent(wakeEvent); }
};

Engine::Engine() : impl_(std::make_unique<Impl>()) {
    impl_->wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    impl_->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    impl_->timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
    if (!impl_->timer) impl_->timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    QueryPerformanceFrequency(&impl_->qpcFreq);

    busBuf_.assign(static_cast<size_t>(kMaxBuses) * 2 * kMaxBlockSize, 0.0f);
    for (int b = 0; b < kMaxBuses; ++b) {
        acc_.l[b] = busBuf_.data() + static_cast<size_t>(b) * 2 * kMaxBlockSize;
        acc_.r[b] = acc_.l[b] + kMaxBlockSize;
    }
}

Engine::~Engine() {
    stop();
    delete current_.exchange(nullptr);
    for (auto& r : retired_) delete r.snap;
    retired_.clear();
    if (impl_->wakeEvent) CloseHandle(impl_->wakeEvent);
    if (impl_->stopEvent) CloseHandle(impl_->stopEvent);
    if (impl_->timer) CloseHandle(impl_->timer);
}

void Engine::setFormat(int sampleRate, int blockSize) {
    if (running()) return;
    sampleRate_ = clampv(sampleRate, 8000, 384000);
    blockSize_ = clampv(blockSize, 16, kMaxBlockSize);
}

WakeTarget* Engine::wakeTarget() { return impl_.get(); }

void Engine::publish(std::unique_ptr<GraphSnapshot> snapshot) {
    GraphSnapshot* old = current_.exchange(snapshot.release(), std::memory_order_acq_rel);
    if (old) retired_.push_back(Retired{old, epoch_.load(std::memory_order_acquire)});
    collectGarbage();
}

void Engine::collectGarbage() {
    if (retired_.empty()) return;
    const bool live = running();
    const uint64_t now = epoch_.load(std::memory_order_acquire);
    auto it = retired_.begin();
    while (it != retired_.end()) {
        // Two completed blocks after the swap, the engine can no longer hold
        // the old pointer.
        if (!live || now >= it->epoch + 2) {
            delete it->snap;
            it = retired_.erase(it);
        } else {
            ++it;
        }
    }
}

bool Engine::start() {
    if (running()) return true;
    stopRequested_.store(false);
    ResetEvent(impl_->stopEvent);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&Engine::threadMain, this);
    return true;
}

void Engine::stop() {
    if (!running()) return;
    stopRequested_.store(true);
    SetEvent(impl_->stopEvent);
    if (thread_.joinable()) thread_.join();
    running_.store(false, std::memory_order_release);
    stats_.clockMode.store(static_cast<int>(ClockMode::Stopped));
    collectGarbage();
}

void Engine::processBlock() {
    GraphSnapshot* g = current_.load(std::memory_order_acquire);
    processBlockImpl(g, blockSize_);
    epoch_.fetch_add(1, std::memory_order_acq_rel);
}

void Engine::processBlockImpl(GraphSnapshot* g, int n) {
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    if (g) {
        try {
            for (int s = 0; s < g->numSources; ++s) g->sources[s]->pull(n);

            const bool talkOpen = micGate_.load(std::memory_order_relaxed);
            bool anySolo = false;
            for (int s = 0; s < g->numStrips; ++s)
                anySolo |= g->strips[s].rt->solo.load(std::memory_order_relaxed);

            std::memset(acc_.used, 0, sizeof(acc_.used));
            for (int b = 0; b < g->numBuses; ++b) {
                const int slot = g->buses[b].slot;
                acc_.used[slot] = true;
                std::memset(acc_.l[slot], 0, sizeof(float) * n);
                std::memset(acc_.r[slot], 0, sizeof(float) * n);
            }

            for (int s = 0; s < g->numStrips; ++s) {
                StripSnap& st = g->strips[s];
                const SourceRuntime* src = st.source >= 0 ? g->sources[st.source] : nullptr;
                const SourceRuntime* ref = st.reference >= 0 ? g->sources[st.reference] : nullptr;
                st.rt->process(src, st.chain, n, anySolo, acc_, ref, talkOpen);
            }

            for (int k = 0; k < g->numSinks; ++k) g->sinks[k]->clear(n);

            for (int b = 0; b < g->numBuses; ++b) {
                BusSnap& bs = g->buses[b];
                float* l = acc_.l[bs.slot];
                float* r = acc_.r[bs.slot];
                bs.rt->process(bs.chain, l, r, n);
                if (bs.sink >= 0) {
                    float* dl = g->sinks[bs.sink]->left();
                    float* dr = g->sinks[bs.sink]->right();
                    for (int i = 0; i < n; ++i) {
                        dl[i] += l[i];
                        dr[i] += r[i];
                    }
                }
            }

            for (int t = 0; t < g->numTaps; ++t) {
                TapSnap& ts = g->taps[t];
                const float* l = nullptr;
                const float* r = nullptr;
                if (ts.kind == RecordTap::Bus && ts.index < g->numBuses) {
                    l = acc_.l[g->buses[ts.index].slot];
                    r = acc_.r[g->buses[ts.index].slot];
                } else if (ts.index < g->numStrips) {
                    StripRuntime* rt = g->strips[ts.index].rt;
                    l = ts.kind == RecordTap::StripPre ? rt->preL() : ts.kind == RecordTap::StripRaw ? rt->rawL() : rt->outL();
                    r = ts.kind == RecordTap::StripPre ? rt->preR() : ts.kind == RecordTap::StripRaw ? rt->rawR() : rt->outR();
                }
                if (!l) continue;
                float* sc = ts.tap->scratch;
                for (int i = 0; i < n; ++i) {
                    sc[2 * i] = l[i];
                    sc[2 * i + 1] = r[i];
                }
                const size_t w = ts.tap->ring.write(sc, static_cast<size_t>(n) * 2);
                ts.tap->capturedFrames.fetch_add(w / 2, std::memory_order_relaxed);
                if (w < static_cast<size_t>(n) * 2)
                    ts.tap->droppedFrames.fetch_add(n - w / 2, std::memory_order_relaxed);
            }

            for (int k = 0; k < g->numSinks; ++k) g->sinks[k]->push(n);
        } catch (...) {
            stats_.exceptions.fetch_add(1, std::memory_order_relaxed);
        }
    }
    QueryPerformanceCounter(&t1);
    const double dt = static_cast<double>(t1.QuadPart - t0.QuadPart) /
                      static_cast<double>(impl_->qpcFreq.QuadPart);
    const float load = static_cast<float>(dt / (static_cast<double>(n) / sampleRate_));
    const float prev = stats_.cpuLoad.load(std::memory_order_relaxed);
    stats_.cpuLoad.store(prev + 0.05f * (load - prev), std::memory_order_relaxed);
    atomicMax(stats_.cpuPeak, load);
    stats_.blockMicros.store(static_cast<float>(dt * 1e6), std::memory_order_relaxed);
    stats_.blocks.fetch_add(1, std::memory_order_relaxed);
    framesProcessed_.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
}

void Engine::threadMain() {
    enableFlushDenormals();
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (mmcss) AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);
    else SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    const double blockSec = blockSeconds();
    const double qpf = static_cast<double>(impl_->qpcFreq.QuadPart);
    ClockMode mode = ClockMode::Stopped;
    LONGLONG timerBase = 0;
    uint64_t timerBlocks = 0;

    auto masterOf = [](GraphSnapshot* g) -> SinkRuntime* {
        return (g && g->masterSink >= 0 && g->masterSink < g->numSinks) ? g->sinks[g->masterSink]
                                                                         : nullptr;
    };

    while (!stopRequested_.load(std::memory_order_acquire)) {
        SinkRuntime* master = masterOf(current_.load(std::memory_order_acquire));
        if (master && master->clockUsable()) {
            if (mode != ClockMode::Device) {
                mode = ClockMode::Device;
                stats_.clockMode.store(static_cast<int>(mode));
                // Kick-start: fill the master's cushion immediately.
                SetEvent(impl_->wakeEvent);
            }
            HANDLE hs[2] = {impl_->stopEvent, impl_->wakeEvent};
            const DWORD timeoutMs = static_cast<DWORD>(std::max(30.0, blockSec * 4000.0));
            const DWORD r = WaitForMultipleObjects(2, hs, FALSE, timeoutMs);
            if (r == WAIT_OBJECT_0) break;
            if (r == WAIT_TIMEOUT) {
                // Device stopped pulling (unplugged, sleeping). Keep the rest
                // of the graph alive; the controller will notice the loss.
                stats_.deviceStalls.fetch_add(1, std::memory_order_relaxed);
                processBlock();
                continue;
            }
            for (int guard = 0; guard < 32; ++guard) {
                SinkRuntime* m = masterOf(current_.load(std::memory_order_acquire));
                if (!m || !m->clockUsable() || !m->clockNeedsData()) break;
                processBlock();
            }
        } else {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            if (mode != ClockMode::Timer) {
                mode = ClockMode::Timer;
                stats_.clockMode.store(static_cast<int>(mode));
                timerBase = now.QuadPart;
                timerBlocks = 0;
            }
            const double elapsed = static_cast<double>(now.QuadPart - timerBase) / qpf;
            const double due = elapsed / blockSec;
            if (static_cast<double>(timerBlocks) + 8.0 < due) {
                const uint64_t target = static_cast<uint64_t>(due);
                stats_.lateBlocks.fetch_add(target - timerBlocks, std::memory_order_relaxed);
                timerBlocks = target;
            }
            if (static_cast<double>(timerBlocks) <= due) {
                processBlock();
                ++timerBlocks;
                continue;
            }
            const double waitSec = static_cast<double>(timerBlocks) * blockSec - elapsed;
            LARGE_INTEGER due100ns;
            due100ns.QuadPart = -static_cast<LONGLONG>(std::max(0.0, waitSec) * 1e7);
            SetWaitableTimer(impl_->timer, &due100ns, 0, nullptr, nullptr, FALSE);
            HANDLE hs[3] = {impl_->stopEvent, impl_->timer, impl_->wakeEvent};
            if (WaitForMultipleObjects(3, hs, FALSE, 100) == WAIT_OBJECT_0) break;
        }
    }
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    stats_.clockMode.store(static_cast<int>(ClockMode::Stopped));
}

} // namespace echo
