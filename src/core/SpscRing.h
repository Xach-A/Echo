// Single-producer / single-consumer lock-free ring buffer.
//
// Exactly one thread may write and exactly one thread may read. Counters are
// monotonically increasing 64-bit values, so wrap-around never happens in
// practice. Capacity is rounded up to a power of two. allocate() must only be
// called while neither side is active.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <vector>

namespace echo {

template <typename T>
class SpscRing {
public:
    SpscRing() = default;
    explicit SpscRing(size_t capacity) { allocate(capacity); }

    void allocate(size_t capacity) {
        size_t cap = 1;
        while (cap < capacity) cap <<= 1;
        buf_.assign(cap, T{});
        mask_ = cap - 1;
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

    size_t capacity() const { return buf_.size(); }

    // Consumer side: elements ready to read.
    size_t readAvailable() const {
        return static_cast<size_t>(head_.load(std::memory_order_acquire) -
                                   tail_.load(std::memory_order_relaxed));
    }

    // Producer side: free space.
    size_t writeAvailable() const {
        return capacity() - static_cast<size_t>(head_.load(std::memory_order_relaxed) -
                                                tail_.load(std::memory_order_acquire));
    }

    // Approximate fill, safe from any thread (used for monitoring).
    size_t size() const {
        const uint64_t t = tail_.load(std::memory_order_acquire);
        const uint64_t h = head_.load(std::memory_order_acquire);
        return h >= t ? static_cast<size_t>(h - t) : 0;
    }

    size_t write(const T* src, size_t n) {
        const uint64_t h = head_.load(std::memory_order_relaxed);
        const uint64_t t = tail_.load(std::memory_order_acquire);
        const size_t space = capacity() - static_cast<size_t>(h - t);
        n = std::min(n, space);
        const size_t start = static_cast<size_t>(h) & mask_;
        const size_t first = std::min(n, capacity() - start);
        std::memcpy(buf_.data() + start, src, first * sizeof(T));
        if (n > first) std::memcpy(buf_.data(), src + first, (n - first) * sizeof(T));
        head_.store(h + n, std::memory_order_release);
        return n;
    }

    // Writes n zero-valued elements (producer side).
    size_t writeZeros(size_t n) {
        const uint64_t h = head_.load(std::memory_order_relaxed);
        const uint64_t t = tail_.load(std::memory_order_acquire);
        const size_t space = capacity() - static_cast<size_t>(h - t);
        n = std::min(n, space);
        const size_t start = static_cast<size_t>(h) & mask_;
        const size_t first = std::min(n, capacity() - start);
        std::fill_n(buf_.data() + start, first, T{});
        if (n > first) std::fill_n(buf_.data(), n - first, T{});
        head_.store(h + n, std::memory_order_release);
        return n;
    }

    size_t read(T* dst, size_t n) {
        const uint64_t t = tail_.load(std::memory_order_relaxed);
        const uint64_t h = head_.load(std::memory_order_acquire);
        n = std::min(n, static_cast<size_t>(h - t));
        const size_t start = static_cast<size_t>(t) & mask_;
        const size_t first = std::min(n, capacity() - start);
        std::memcpy(dst, buf_.data() + start, first * sizeof(T));
        if (n > first) std::memcpy(dst + first, buf_.data(), (n - first) * sizeof(T));
        tail_.store(t + n, std::memory_order_release);
        return n;
    }

    // Consumer side: drop up to n elements.
    size_t skip(size_t n) {
        const uint64_t t = tail_.load(std::memory_order_relaxed);
        const uint64_t h = head_.load(std::memory_order_acquire);
        n = std::min(n, static_cast<size_t>(h - t));
        tail_.store(t + n, std::memory_order_release);
        return n;
    }

private:
    std::vector<T> buf_;
    size_t mask_ = 0;
    alignas(64) std::atomic<uint64_t> head_{0};
    alignas(64) std::atomic<uint64_t> tail_{0};
};

} // namespace echo
