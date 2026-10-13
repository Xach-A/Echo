#include "ai/FaissIndex.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

namespace echo::ai {

namespace {

class Reader {
public:
    explicit Reader(FILE* f) : f_(f) {}
    template <typename T>
    bool read(T& v) {
        return std::fread(&v, sizeof(T), 1, f_) == 1;
    }
    bool bytes(void* p, size_t n) { return n == 0 || std::fread(p, 1, n, f_) == n; }
    bool skip(uint64_t n) { return _fseeki64(f_, static_cast<int64_t>(n), SEEK_CUR) == 0; }
    bool fourcc(const char* expect) {
        char c[4];
        return bytes(c, 4) && std::memcmp(c, expect, 4) == 0;
    }

private:
    FILE* f_;
};

struct Header {
    int32_t d = 0;
    int64_t ntotal = 0;
    int32_t metric = 0;
};

bool readHeader(Reader& r, Header& h) {
    int64_t dummy;
    uint8_t trained;
    if (!r.read(h.d) || !r.read(h.ntotal) || !r.read(dummy) || !r.read(dummy) || !r.read(trained) || !r.read(h.metric)) return false;
    if (h.metric > 1) {
        float arg;
        if (!r.read(arg)) return false;
    }
    return h.d > 0 && h.d <= 4096 && h.ntotal >= 0;
}

// A flat (IxF2) index's stored vectors.
bool readFlat(Reader& r, const Header& h, std::vector<float>& out) {
    uint64_t count;
    if (!r.read(count) || count != static_cast<uint64_t>(h.ntotal) * static_cast<uint64_t>(h.d) || count > (1ull << 32)) return false;
    out.resize(static_cast<size_t>(count));
    return r.bytes(out.data(), out.size() * 4);
}

} // namespace

bool FaissIndex::load(const std::wstring& path, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        dim_ = 0;
        return false;
    };
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return fail("Can't open the index file");
    struct Closer {
        FILE* f;
        ~Closer() { std::fclose(f); }
    } closer{f};
    Reader r(f);
    char kind[4];
    if (!r.bytes(kind, 4)) return fail("Empty index file");
    Header h;
    if (std::memcmp(kind, "IxF2", 4) == 0) {
        if (!readHeader(r, h) || h.metric != 1) return fail("Unsupported index (not L2)");
        std::vector<float> all;
        if (!readFlat(r, h, all)) return fail("Damaged index file");
        lists_.assign(1, std::move(all));
        centroids_.clear();
        dim_ = h.d;
        total_ = h.ntotal;
        return true;
    }
    if (std::memcmp(kind, "IwFl", 4) != 0) return fail("Unsupported index type (Echo reads IVF-Flat and flat indexes)");
    if (!readHeader(r, h) || h.metric != 1) return fail("Unsupported index (not L2)");
    uint64_t nlist, nprobe;
    if (!r.read(nlist) || !r.read(nprobe) || nlist == 0 || nlist > (1u << 24)) return fail("Damaged index file");
    // The coarse quantizer: one centroid per cluster.
    Header q;
    if (!r.fourcc("IxF2") || !readHeader(r, q) || q.d != h.d || static_cast<uint64_t>(q.ntotal) != nlist || !readFlat(r, q, centroids_))
        return fail("Unsupported index (its clusters are not flat L2)");
    // Direct map (id -> position), not needed here.
    uint8_t mapType;
    uint64_t mapSize;
    if (!r.read(mapType) || !r.read(mapSize) || !r.skip(mapSize * 8)) return fail("Damaged index file");
    if (mapType == 2) {
        uint64_t pairs;
        if (!r.read(pairs) || !r.skip(pairs * 16)) return fail("Damaged index file");
    }
    // Inverted lists: the vectors of each cluster.
    uint64_t lists, codeSize;
    char listType[4];
    if (!r.fourcc("ilar") || !r.read(lists) || !r.read(codeSize) || lists != nlist || codeSize != static_cast<uint64_t>(h.d) * 4 ||
        !r.bytes(listType, 4))
        return fail("Unsupported index (its lists are not stored as plain vectors)");
    uint64_t n;
    if (!r.read(n) || n > (1ull << 26)) return fail("Damaged index file");
    std::vector<uint64_t> sizes(static_cast<size_t>(n));
    if (!r.bytes(sizes.data(), sizes.size() * 8)) return fail("Damaged index file");
    std::vector<uint64_t> perList(static_cast<size_t>(nlist), 0);
    if (std::memcmp(listType, "full", 4) == 0) {
        if (n != nlist) return fail("Damaged index file");
        perList = sizes;
    } else if (std::memcmp(listType, "sprs", 4) == 0) {
        for (size_t k = 0; k + 1 < sizes.size(); k += 2) {
            if (sizes[k] >= nlist) return fail("Damaged index file");
            perList[static_cast<size_t>(sizes[k])] = sizes[k + 1];
        }
    } else {
        return fail("Damaged index file");
    }
    lists_.assign(static_cast<size_t>(nlist), {});
    uint64_t sum = 0;
    for (size_t l = 0; l < perList.size(); ++l) {
        const uint64_t count = perList[l];
        if (count == 0) continue;
        sum += count;
        if (sum > static_cast<uint64_t>(h.ntotal)) return fail("Damaged index file");
        lists_[l].resize(static_cast<size_t>(count * static_cast<uint64_t>(h.d)));
        if (!r.bytes(lists_[l].data(), lists_[l].size() * 4) || !r.skip(count * 8)) return fail("Damaged index file");
    }
    dim_ = h.d;
    total_ = h.ntotal;
    return true;
}

namespace {

// Squared L2 distance, summed in 8 lanes (the compiler can vectorize that).
float sqDist(const float* a, const float* b, size_t d) {
    float acc[8] = {};
    size_t i = 0;
    for (; i + 8 <= d; i += 8)
        for (size_t j = 0; j < 8; ++j) {
            const float e = a[i + j] - b[i + j];
            acc[j] += e * e;
        }
    float s = ((acc[0] + acc[1]) + (acc[2] + acc[3])) + ((acc[4] + acc[5]) + (acc[6] + acc[7]));
    for (; i < d; ++i) {
        const float e = a[i] - b[i];
        s += e * e;
    }
    return s;
}

// Keeps the `n` smallest (score, value) pairs (a max-heap).
template <typename V>
void keepSmallest(std::vector<std::pair<float, V>>& heap, size_t n, float score, V value) {
    if (heap.size() < n) {
        heap.emplace_back(score, value);
        std::push_heap(heap.begin(), heap.end());
    } else if (score < heap.front().first) {
        std::pop_heap(heap.begin(), heap.end());
        heap.back() = {score, value};
        std::push_heap(heap.begin(), heap.end());
    }
}

} // namespace

void FaissIndex::retrieve(const float* query, int k, float* out, int probe) const { retrieveMany(query, 1, k, out, probe); }

void FaissIndex::retrieveMany(const float* queries, int count, int k, float* out, int probe) const {
    const size_t d = static_cast<size_t>(dim_);
    if (count <= 0 || d == 0) return;
    // The clusters each query searches: its nearest centroids, found in one
    // pass over the centroids for all queries (they stay in the cache).
    const size_t nlist = centroids_.empty() ? 1 : centroids_.size() / d;
    const size_t p = std::min(nlist, static_cast<size_t>(std::max(1, probe)));
    std::vector<std::vector<std::pair<float, size_t>>> near(static_cast<size_t>(count));
    if (centroids_.empty()) {
        for (auto& n : near) n.emplace_back(0.0f, 0);
    } else {
        for (size_t l = 0; l < nlist; ++l) {
            const float* c = &centroids_[l * d];
            for (int i = 0; i < count; ++i)
                keepSmallest(near[static_cast<size_t>(i)], p, sqDist(c, queries + static_cast<size_t>(i) * d, d), l);
        }
    }
    std::vector<std::pair<float, const float*>> best;
    std::vector<double> w;
    for (int i = 0; i < count; ++i) {
        const float* query = queries + static_cast<size_t>(i) * d;
        float* o = out + static_cast<size_t>(i) * d;
        // The k nearest vectors (squared L2) in those clusters.
        best.clear();
        for (const auto& cl : near[static_cast<size_t>(i)]) {
            const auto& list = lists_[cl.second];
            for (size_t v = 0; v + d <= list.size(); v += d) keepSmallest(best, static_cast<size_t>(k), sqDist(&list[v], query, d), &list[v]);
        }
        if (best.empty()) {
            std::memcpy(o, query, d * sizeof(float));
            continue;
        }
        // Weights 1 / distance^2 (an exact match takes all the weight).
        w.assign(best.size(), 0.0);
        double total = 0.0;
        for (size_t b = 0; b < best.size(); ++b) {
            const double s = std::max(static_cast<double>(best[b].first), 1e-12);
            w[b] = 1.0 / (s * s);
            total += w[b];
        }
        for (size_t j = 0; j < d; ++j) {
            double acc = 0.0;
            for (size_t b = 0; b < best.size(); ++b) acc += w[b] * best[b].second[j];
            o[j] = static_cast<float>(acc / total);
        }
    }
}

} // namespace echo::ai
