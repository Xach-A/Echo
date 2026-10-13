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

void FaissIndex::retrieve(const float* query, int k, float* out, int probe) const {
    const size_t d = static_cast<size_t>(dim_);
    auto dist = [&](const float* v) {
        float s = 0.0f;
        for (size_t i = 0; i < d; ++i) {
            const float e = v[i] - query[i];
            s += e * e;
        }
        return s;
    };
    // Clusters to search.
    std::vector<size_t> search;
    if (centroids_.empty()) {
        search.push_back(0);
    } else {
        const size_t nlist = centroids_.size() / d;
        std::vector<std::pair<float, size_t>> c(nlist);
        for (size_t l = 0; l < nlist; ++l) c[l] = {dist(&centroids_[l * d]), l};
        const size_t p = std::min<size_t>(nlist, static_cast<size_t>(std::max(1, probe)));
        std::partial_sort(c.begin(), c.begin() + static_cast<long>(p), c.end());
        for (size_t i = 0; i < p; ++i) search.push_back(c[i].second);
    }
    // The k nearest vectors (squared L2).
    std::vector<std::pair<float, const float*>> best;
    for (size_t l : search) {
        const auto& list = lists_[l];
        for (size_t v = 0; v + d <= list.size(); v += d) {
            const float s = dist(&list[v]);
            if (static_cast<int>(best.size()) < k) {
                best.emplace_back(s, &list[v]);
                std::push_heap(best.begin(), best.end());
            } else if (s < best.front().first) {
                std::pop_heap(best.begin(), best.end());
                best.back() = {s, &list[v]};
                std::push_heap(best.begin(), best.end());
            }
        }
    }
    if (best.empty()) {
        std::memcpy(out, query, d * sizeof(float));
        return;
    }
    // Weights 1 / distance^2 (an exact match takes all the weight).
    std::vector<double> w(best.size());
    double total = 0.0;
    for (size_t i = 0; i < best.size(); ++i) {
        const double s = std::max(static_cast<double>(best[i].first), 1e-12);
        w[i] = 1.0 / (s * s);
        total += w[i];
    }
    for (size_t i = 0; i < d; ++i) {
        double acc = 0.0;
        for (size_t b = 0; b < best.size(); ++b) acc += w[b] * best[b].second[i];
        out[i] = static_cast<float>(acc / total);
    }
}

} // namespace echo::ai
