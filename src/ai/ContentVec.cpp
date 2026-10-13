#include "ai/ContentVec.h"

#include <algorithm>
#include <cmath>

#include "ai/OnnxGraph.h"

namespace echo::ai {

namespace {

constexpr int kHeadDim = 64; // ContentVec: 12 heads of 64
constexpr int kConvKernel[7] = {10, 3, 3, 3, 3, 2, 2};
constexpr int kConvStride[7] = {5, 2, 2, 2, 2, 2, 2};

} // namespace

bool ContentVecModel::load(const std::wstring& bin, std::string* error) {
    if (!ck_.open(bin, error)) return false;
    tensors_ = TorchCheckpoint::tensors(ck_.root());
    auto proj = tensors_.find("feature_projection.projection.weight");
    if (!tensors_.count("feature_extractor.conv_layers.0.conv.weight") || proj == tensors_.end() || proj->second.shape.size() != 2 ||
        !tensors_.count("encoder.layers.0.final_layer_norm.weight")) {
        if (error) *error = "This file is not the ContentVec model Echo expects";
        return false;
    }
    dim_ = static_cast<int>(proj->second.shape[0]);
    layers_ = 0;
    while (tensors_.count("encoder.layers." + std::to_string(layers_) + ".final_layer_norm.weight")) ++layers_;
    return true;
}

int ContentVecModel::framesFor(int samples) {
    int n = samples;
    for (int l = 0; l < 7; ++l) n = n < kConvKernel[l] ? 0 : (n - kConvKernel[l]) / kConvStride[l] + 1;
    return n;
}

bool ContentVecModel::param(const std::string& name, W& out) const {
    auto read = [&](const std::string& key, W& w) {
        auto it = tensors_.find(key);
        if (it == tensors_.end()) return false;
        w.shape = it->second.shape;
        std::string e;
        return ck_.read(it->second, w.v, &e);
    };
    if (read(name, out)) return true;
    // The positional convolution's weight norm runs over the kernel axis
    // (dim 2): w[o, i, k] = g[k] * v[o, i, k] / |v[:, :, k]|.
    const std::string base = name.size() > 7 ? name.substr(0, name.size() - 7) : name;
    W g, v;
    if (!(read(base + ".weight_g", g) && read(base + ".weight_v", v)) &&
        !(read(base + ".parametrizations.weight.original0", g) && read(base + ".parametrizations.weight.original1", v)))
        return false;
    if (v.shape.size() != 3 || g.v.size() != static_cast<size_t>(v.shape[2])) return false;
    const size_t K = static_cast<size_t>(v.shape[2]), rows = v.v.size() / K;
    std::vector<double> norm(K, 0.0);
    for (size_t r = 0; r < rows; ++r)
        for (size_t k = 0; k < K; ++k) norm[k] += static_cast<double>(v.v[r * K + k]) * v.v[r * K + k];
    out.shape = v.shape;
    out.v.resize(v.v.size());
    for (size_t r = 0; r < rows; ++r)
        for (size_t k = 0; k < K; ++k)
            out.v[r * K + k] = norm[k] > 0.0 ? static_cast<float>(g.v[k] * v.v[r * K + k] / std::sqrt(norm[k])) : 0.0f;
    return true;
}

bool ContentVecModel::buildGraph(int samples, int layer, bool finalProj, std::vector<uint8_t>& out, std::string* error) const {
    const int64_t F = framesFor(samples);
    if (layer <= 0 || layer > layers_) layer = layers_;
    if (F < 1) {
        if (error) *error = "Too little audio for the speech encoder";
        return false;
    }
    std::string missing;
    OnnxGraph g;
    auto need = [&](const std::string& name) {
        W w;
        if (!param(name, w) && missing.empty()) missing = name;
        return w;
    };
    auto constant = [&](const W& w) { return g.constant(w.shape, w.v); };
    auto gelu = [&](const std::string& x) { // exact (erf) GELU
        const std::string e = g.op("Erf", {g.op("Mul", {x, g.scalar(static_cast<float>(1.0 / std::sqrt(2.0)))})});
        return g.op("Mul", {g.op("Mul", {x, g.scalar(0.5f)}), g.op("Add", {e, g.scalar(1.0f)})});
    };
    // x [.., in] -> [.., out] with a [out, in] weight.
    auto linear = [&](const std::string& x, const std::string& prefix) {
        const W w = need(prefix + ".weight"), b = need(prefix + ".bias");
        if (w.shape.size() != 2) return x;
        const int64_t o = w.shape[0], in = w.shape[1];
        std::vector<float> t(w.v.size());
        for (int64_t r = 0; r < o; ++r)
            for (int64_t c = 0; c < in; ++c) t[static_cast<size_t>(c * o + r)] = w.v[static_cast<size_t>(r * in + c)];
        return g.op("Add", {g.op("MatMul", {x, g.constant({in, o}, t)}), constant(b)});
    };
    auto layerNorm = [&](const std::string& x, const std::string& prefix) {
        return g.op("LayerNormalization", {x, constant(need(prefix + ".weight")), constant(need(prefix + ".bias"))},
                    {attrInt("axis", -1), attrFloat("epsilon", 1e-5f)});
    };

    const std::string audio = g.input("audio", OnnxType::Float, {1, samples});
    std::string x = g.op("Unsqueeze", {audio, g.ints({1})}); // [1,1,N]
    for (int l = 0; l < 7; ++l) {
        const std::string p = "feature_extractor.conv_layers." + std::to_string(l);
        x = g.op("Conv", {x, constant(need(p + ".conv.weight"))},
                 {attrInts("kernel_shape", {kConvKernel[l]}), attrInts("strides", {kConvStride[l]})});
        if (l == 0) // GroupNorm with a group per channel
            x = g.op("InstanceNormalization", {x, constant(need(p + ".layer_norm.weight")), constant(need(p + ".layer_norm.bias"))},
                     {attrFloat("epsilon", 1e-5f)});
        x = gelu(x);
    }
    std::string h = g.op("Transpose", {x}, {attrInts("perm", {0, 2, 1})}); // [1,F,512]
    h = linear(layerNorm(h, "feature_projection.layer_norm"), "feature_projection.projection");
    {
        const W pw = need("encoder.pos_conv_embed.conv.weight"), pb = need("encoder.pos_conv_embed.conv.bias");
        const int64_t k = pw.shape.size() == 3 ? pw.shape[2] : 128;
        std::string pos = g.op("Transpose", {h}, {attrInts("perm", {0, 2, 1})});
        pos = g.op("Conv", {pos, constant(pw), constant(pb)},
                   {attrInts("kernel_shape", {k}), attrInts("pads", {k / 2, k / 2}), attrInt("group", 16)});
        if (k % 2 == 0) pos = g.op("Slice", {pos, g.ints({0}), g.ints({F}), g.ints({2})}); // same padding
        pos = g.op("Transpose", {gelu(pos)}, {attrInts("perm", {0, 2, 1})});
        h = layerNorm(g.op("Add", {h, pos}), "encoder.layer_norm");
    }
    const int64_t D = dim_, dk = kHeadDim, kHeads = std::max<int64_t>(1, D / kHeadDim);
    const float scale = 1.0f / std::sqrt(static_cast<float>(dk));
    for (int l = 0; l < layer; ++l) {
        const std::string p = "encoder.layers." + std::to_string(l);
        auto heads = [&](const std::string& v) {
            return g.op("Transpose", {g.op("Reshape", {v, g.ints({1, F, kHeads, dk})})}, {attrInts("perm", {0, 2, 1, 3})});
        };
        const std::string q = heads(g.op("Mul", {linear(h, p + ".attention.q_proj"), g.scalar(scale)}));
        const std::string k = heads(linear(h, p + ".attention.k_proj"));
        const std::string v = heads(linear(h, p + ".attention.v_proj"));
        const std::string s =
            g.op("Softmax", {g.op("MatMul", {q, g.op("Transpose", {k}, {attrInts("perm", {0, 1, 3, 2})})})}, {attrInt("axis", -1)});
        std::string o = g.op("Transpose", {g.op("MatMul", {s, v})}, {attrInts("perm", {0, 2, 1, 3})});
        o = linear(g.op("Reshape", {o, g.ints({1, F, D})}), p + ".attention.out_proj");
        h = layerNorm(g.op("Add", {h, o}), p + ".layer_norm");
        const std::string ff = linear(gelu(linear(h, p + ".feed_forward.intermediate_dense")), p + ".feed_forward.output_dense");
        h = layerNorm(g.op("Add", {h, ff}), p + ".final_layer_norm");
    }
    int64_t dim = D;
    if (finalProj) {
        const auto fp = tensors_.find("final_proj.weight");
        if (fp == tensors_.end() || fp->second.shape.size() != 2) {
            if (error) *error = "The ContentVec model has no projection for RVC v1 voices";
            return false;
        }
        h = linear(h, "final_proj");
        dim = fp->second.shape[0];
    }
    g.output(h, "features", OnnxType::Float, {1, F, dim});
    if (!missing.empty()) {
        if (error) *error = "The ContentVec model is missing '" + missing + "'";
        return false;
    }
    out = g.serialize();
    return true;
}

} // namespace echo::ai
