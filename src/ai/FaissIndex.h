// RVC's .index files: a FAISS index of the speech features the voice was
// trained on. Blending each frame's features towards its nearest training
// features makes the result sound more like the voice. Reads IVF-Flat
// ("IwFl") and flat ("IxF2") indexes, the kinds RVC and Applio write.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace echo::ai {

class FaissIndex {
public:
    bool load(const std::wstring& path, std::string* error);
    bool loaded() const { return dim_ > 0; }
    int dim() const { return dim_; }
    int64_t size() const { return total_; }

    // RVC's retrieval: the k nearest stored vectors (searching the nearest
    // `probe` clusters), weighted by 1 / distance^2, averaged into `out`.
    void retrieve(const float* query, int k, float* out, int probe = 1) const;
    // The same for `count` queries ([count, dim], out likewise), much faster
    // than one at a time: the centroids are read once for all of them.
    void retrieveMany(const float* queries, int count, int k, float* out, int probe = 1) const;

private:
    int dim_ = 0;
    int64_t total_ = 0;
    std::vector<float> centroids_;            // nlist x dim (empty for a flat index)
    std::vector<std::vector<float>> lists_;   // vectors per cluster
};

} // namespace echo::ai
