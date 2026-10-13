#include "ai/RvcModel.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "ai/OnnxGraph.h"

namespace echo::ai {

namespace {

constexpr float kLrelu = 0.1f;   // LRELU_SLOPE
constexpr int kWindow = 10;      // relative attention window
constexpr int kFlowLayers = 3;   // WaveNet layers per coupling
constexpr int kFlowKernel = 5;
constexpr int kFlows = 4;

std::vector<int> intList(const PyRef& v) {
    std::vector<int> out;
    if (v)
        for (const auto& i : v->items) out.push_back(static_cast<int>(i->number()));
    return out;
}

} // namespace

int64_t rvcCoarsePitch(float hz) {
    static const double melMin = 1127.0 * std::log(1.0 + 50.0 / 700.0), melMax = 1127.0 * std::log(1.0 + 1100.0 / 700.0);
    double mel = 1127.0 * std::log(1.0 + hz / 700.0);
    if (mel > 0.0) mel = (mel - melMin) * 254.0 / (melMax - melMin) + 1.0;
    if (mel <= 1.0) mel = 1.0;
    if (mel > 255.0) mel = 255.0;
    return static_cast<int64_t>(std::nearbyint(mel));
}

bool RvcModel::load(const std::wstring& pth, std::string* error) {
    if (!ck_.open(pth, error)) return false;
    const PyRef& root = ck_.root();
    const PyRef weights = root ? root->get("weight") : nullptr;
    const PyRef config = root ? root->get("config") : nullptr;
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (!weights || !config || config->items.size() < 18)
        return fail("This .pth is not an RVC voice model (it has no voice weights and settings)");
    tensors_ = TorchCheckpoint::tensors(weights);
    const auto& c = config->items;
    RvcInfo& i = info_;
    i.inter = static_cast<int>(c[2]->number());
    i.hidden = static_cast<int>(c[3]->number());
    i.filter = static_cast<int>(c[4]->number());
    i.heads = static_cast<int>(c[5]->number());
    i.layers = static_cast<int>(c[6]->number());
    i.kernel = static_cast<int>(c[7]->number());
    i.rbKernels = intList(c[10]);
    for (const auto& d : c[11]->items) i.rbDilations.push_back(intList(d));
    i.upRates = intList(c[12]);
    i.upInitial = static_cast<int>(c[13]->number());
    i.upKernels = intList(c[14]);
    i.speakers = static_cast<int>(c[15]->number());
    i.gin = static_cast<int>(c[16]->number());
    i.sampleRate = c[17]->isNumber() ? static_cast<int>(c[17]->number()) : 0;
    auto str = [&](const char* key, const char* def) {
        const PyRef v = root->get(key);
        return v && v->isStr() ? v->s : std::string(def);
    };
    i.version = str("version", "v1");
    i.vocoder = str("vocoder", "HiFi-GAN");
    i.embedder = str("embedder_model", "contentvec");
    i.name = str("model_name", "");
    const PyRef f0 = root->get("f0");
    i.f0 = !f0 || f0->number() != 0.0;
    i.hop = 1;
    for (int r : i.upRates) i.hop *= r;
    if (auto it = tensors_.find("enc_p.emb_phone.weight"); it != tensors_.end() && it->second.shape.size() == 2)
        i.embDim = static_cast<int>(it->second.shape[1]);
    if (!i.f0) return fail("This RVC model has no pitch guidance; Echo supports models trained with pitch (most are)");
    if (i.vocoder != "HiFi-GAN") return fail("This RVC model uses the " + i.vocoder + " vocoder; Echo supports HiFi-GAN models");
    if (i.sampleRate <= 0 || i.hop * 100 != i.sampleRate || i.upRates.size() != i.upKernels.size() || i.rbKernels.empty() ||
        i.rbKernels.size() != i.rbDilations.size() || i.heads <= 0 || i.hidden % i.heads)
        return fail("This RVC model's settings are not supported");
    if (i.embedder != "contentvec" && i.embedder != "hubert")
        return fail("This RVC model was trained on the '" + i.embedder + "' speech features; Echo supports ContentVec models");
    return true;
}

bool RvcModel::param(const std::string& name, W& out) const {
    auto read = [&](const std::string& key, W& w) {
        auto it = tensors_.find(key);
        if (it == tensors_.end()) return false;
        w.shape = it->second.shape;
        std::string e;
        return ck_.read(it->second, w.v, &e);
    };
    if (read(name, out)) return true;
    // Weight norm: w = g * v / |v|, the norm over all but the first axis.
    const std::string base = name.size() > 7 && name.compare(name.size() - 7, 7, ".weight") == 0 ? name.substr(0, name.size() - 7) : name;
    W g, v;
    if (!(read(base + ".weight_g", g) && read(base + ".weight_v", v)) &&
        !(read(base + ".parametrizations.weight.original0", g) && read(base + ".parametrizations.weight.original1", v)))
        return false;
    const size_t rows = static_cast<size_t>(v.shape.empty() ? 1 : v.shape[0]);
    if (g.v.size() != rows || v.v.empty() || v.v.size() % rows) return false;
    const size_t per = v.v.size() / rows;
    out.shape = v.shape;
    out.v.resize(v.v.size());
    for (size_t r = 0; r < rows; ++r) {
        double n = 0.0;
        for (size_t k = 0; k < per; ++k) n += static_cast<double>(v.v[r * per + k]) * v.v[r * per + k];
        const float s = n > 0.0 ? static_cast<float>(g.v[r] / std::sqrt(n)) : 0.0f;
        for (size_t k = 0; k < per; ++k) out.v[r * per + k] = v.v[r * per + k] * s;
    }
    return true;
}

bool RvcModel::buildGenerator(int frames, int speaker, std::vector<uint8_t>& out, std::string* error, int decodeFrames) const {
    const RvcInfo& I = info_;
    const int64_t T = frames, upp = I.hop, C = I.hidden, H = I.heads, dk = C / H, inter = I.inter;
    // Frames the flow runs on (Tf) and the vocoder makes (D): the newest.
    const int64_t D = decodeFrames > 0 && decodeFrames < frames ? decodeFrames : T;
    const int64_t Tf = flowFrames(frames, static_cast<int>(D));
    std::string missing;
    OnnxGraph g;
    auto need = [&](const std::string& name) {
        W w;
        if (!param(name, w) && missing.empty()) missing = name;
        return w;
    };
    auto constant = [&](const W& w) { return g.constant(w.shape, w.v); };
    // Conv1d; `extraBias` is added to the bias (folded constants).
    auto conv = [&](const std::string& x, const std::string& prefix, int pad, int dil = 1, int stride = 1, bool bias = true,
                    const std::vector<float>* extraBias = nullptr) {
        const W w = need(prefix + ".weight");
        if (w.shape.size() != 3) return x;
        std::vector<std::string> in = {x, constant(w)};
        if (bias || extraBias) {
            W b;
            if (bias) b = need(prefix + ".bias");
            b.v.resize(static_cast<size_t>(w.shape[0]), 0.0f);
            if (extraBias)
                for (size_t k = 0; k < b.v.size() && k < extraBias->size(); ++k) b.v[k] += (*extraBias)[k];
            in.push_back(g.constant({w.shape[0]}, b.v));
        }
        return g.op("Conv", in,
                    {attrInts("kernel_shape", {w.shape[2]}), attrInts("pads", {pad, pad}), attrInts("dilations", {dil}),
                     attrInts("strides", {stride})});
    };
    auto lrelu = [&](const std::string& x, float slope) { return g.op("LeakyRelu", {x}, {attrFloat("alpha", slope)}); };
    auto transpose = [&](const std::string& x, std::vector<int64_t> perm) { return g.op("Transpose", {x}, {attrInts("perm", std::move(perm))}); };
    auto reshape = [&](const std::string& x, std::vector<int64_t> shape) { return g.op("Reshape", {x, g.ints(shape)}); };
    auto split2 = [&](const std::string& x, int64_t a, int64_t b, int64_t axis) {
        return g.opN("Split", {x, g.ints({a, b})}, 2, {attrInt("axis", axis)});
    };
    auto layerNorm = [&](const std::string& x, const std::string& prefix) {
        const W gamma = need(prefix + ".gamma"), beta = need(prefix + ".beta");
        const std::string t = transpose(x, {0, 2, 1});
        const std::string n = g.op("LayerNormalization", {t, constant(gamma), constant(beta)},
                                   {attrInt("axis", -1), attrFloat("epsilon", 1e-5f)});
        return transpose(n, {0, 2, 1});
    };
    // Matrix-vector product of a [out, in, 1] conv weight with v, plus bias.
    auto applyConv1x1 = [&](const std::string& prefix, const std::vector<float>& v) {
        const W w = need(prefix + ".weight"), b = need(prefix + ".bias");
        std::vector<float> r(static_cast<size_t>(w.shape.empty() ? 0 : w.shape[0]), 0.0f);
        const size_t in = v.size();
        for (size_t o = 0; o < r.size() && w.v.size() >= (o + 1) * in; ++o) {
            double s = o < b.v.size() ? b.v[o] : 0.0;
            for (size_t k = 0; k < in; ++k) s += static_cast<double>(w.v[o * in + k]) * v[k];
            r[o] = static_cast<float>(s);
        }
        return r;
    };

    // Speaker embedding (fixed for the graph).
    const W embG = need("emb_g.weight");
    std::vector<float> spk(static_cast<size_t>(I.gin), 0.0f);
    if (embG.shape.size() == 2 && speaker >= 0 && speaker < embG.shape[0])
        for (int k = 0; k < I.gin; ++k) spk[static_cast<size_t>(k)] = embG.v[static_cast<size_t>(speaker * I.gin + k)];

    // ------------------------------------------------------------ inputs
    const std::string phone = g.input("phone", OnnxType::Float, {1, T, I.embDim});
    const std::string pitch = g.input("pitch", OnnxType::Int64, {1, T});
    const std::string pitchf = g.input("pitchf", OnnxType::Float, {1, T});
    const std::string rnd = g.input("rnd", OnnxType::Float, {1, inter, Tf});
    const std::string noise = g.input("noise", OnnxType::Float, {1, D * upp});
    // The newest `keep` of `len` along `axis`.
    auto newest = [&](const std::string& x, int64_t len, int64_t keep, int64_t axis) {
        return keep >= len ? x : g.op("Slice", {x, g.ints({len - keep}), g.ints({len}), g.ints({axis})});
    };

    // ------------------------------------------------------ text encoder
    {
        // Relative attention index maps for this length.
        std::vector<int64_t> idxK(static_cast<size_t>(H * T * T)), idxV(static_cast<size_t>(H * T * (2 * kWindow + 1)));
        std::vector<float> maskK(static_cast<size_t>(T * T)), maskV(static_cast<size_t>(T * (2 * kWindow + 1)));
        for (int64_t i = 0; i < T; ++i) {
            for (int64_t j = 0; j < T; ++j) {
                const int64_t r = j - i + kWindow;
                maskK[static_cast<size_t>(i * T + j)] = (r >= 0 && r <= 2 * kWindow) ? 1.0f : 0.0f;
                for (int64_t h = 0; h < H; ++h)
                    idxK[static_cast<size_t>((h * T + i) * T + j)] = std::clamp<int64_t>(r, 0, 2 * kWindow);
            }
            for (int64_t r = 0; r <= 2 * kWindow; ++r) {
                const int64_t j = i + r - kWindow;
                maskV[static_cast<size_t>(i * (2 * kWindow + 1) + r)] = (j >= 0 && j < T) ? 1.0f : 0.0f;
                for (int64_t h = 0; h < H; ++h)
                    idxV[static_cast<size_t>((h * T + i) * (2 * kWindow + 1) + r)] = std::clamp<int64_t>(j, 0, T - 1);
            }
        }
        const std::string cIdxK = g.constantInt64({1, H, T, T}, idxK), cIdxV = g.constantInt64({1, H, T, 2 * kWindow + 1}, idxV);
        const std::string cMaskK = g.constant({1, 1, T, T}, maskK), cMaskV = g.constant({1, 1, T, 2 * kWindow + 1}, maskV);

        W embPhone = need("enc_p.emb_phone.weight"); // [C, embDim]
        std::vector<float> wt(embPhone.v.size());
        for (int64_t o = 0; o < C; ++o)
            for (int64_t k = 0; k < I.embDim && static_cast<size_t>(o * I.embDim + k) < embPhone.v.size(); ++k)
                wt[static_cast<size_t>(k * C + o)] = embPhone.v[static_cast<size_t>(o * I.embDim + k)];
        std::string x = g.op("MatMul", {phone, g.constant({I.embDim, C}, wt)});
        x = g.op("Add", {x, constant(need("enc_p.emb_phone.bias"))});
        x = g.op("Add", {x, g.op("Gather", {constant(need("enc_p.emb_pitch.weight")), pitch}, {attrInt("axis", 0)})});
        x = g.op("Mul", {x, g.scalar(std::sqrt(static_cast<float>(C)))});
        x = lrelu(x, kLrelu);
        x = transpose(x, {0, 2, 1}); // [1, C, T]
        const float qScale = 1.0f / std::sqrt(static_cast<float>(dk));
        for (int l = 0; l < I.layers; ++l) {
            const std::string p = "enc_p.encoder.attn_layers." + std::to_string(l);
            auto heads = [&](const std::string& v) { return transpose(reshape(v, {1, H, dk, T}), {0, 1, 3, 2}); }; // [1,H,T,dk]
            const std::string q = g.op("Mul", {heads(conv(x, p + ".conv_q", 0)), g.scalar(qScale)});
            const std::string k = heads(conv(x, p + ".conv_k", 0));
            const std::string v = heads(conv(x, p + ".conv_v", 0));
            std::string scores = g.op("MatMul", {q, transpose(k, {0, 1, 3, 2})});
            // Relative keys: q . E_k[j - i + window] within the window.
            W relK = need(p + ".emb_rel_k"), relV = need(p + ".emb_rel_v"); // [1, 2w+1, dk]
            std::vector<float> relKt(relK.v.size());
            for (int64_t r = 0; r <= 2 * kWindow; ++r)
                for (int64_t d = 0; d < dk && static_cast<size_t>(r * dk + d) < relK.v.size(); ++d)
                    relKt[static_cast<size_t>(d * (2 * kWindow + 1) + r)] = relK.v[static_cast<size_t>(r * dk + d)];
            const std::string rel = g.op("MatMul", {q, g.constant({dk, 2 * kWindow + 1}, relKt)}); // [1,H,T,2w+1]
            const std::string relAbs = g.op("Mul", {g.op("GatherElements", {rel, cIdxK}, {attrInt("axis", 3)}), cMaskK});
            scores = g.op("Add", {scores, relAbs});
            const std::string pAttn = g.op("Softmax", {scores}, {attrInt("axis", -1)});
            std::string o = g.op("MatMul", {pAttn, v});
            // Relative values: sum over the window of p[i, i + d] E_v[d + window].
            const std::string rw = g.op("Mul", {g.op("GatherElements", {pAttn, cIdxV}, {attrInt("axis", 3)}), cMaskV});
            o = g.op("Add", {o, g.op("MatMul", {rw, g.constant({2 * kWindow + 1, dk}, relV.v)})});
            o = reshape(transpose(o, {0, 1, 3, 2}), {1, C, T});
            const std::string y = conv(o, p + ".conv_o", 0);
            x = layerNorm(g.op("Add", {x, y}), "enc_p.encoder.norm_layers_1." + std::to_string(l));
            const std::string f = "enc_p.encoder.ffn_layers." + std::to_string(l);
            const int pad = (I.kernel - 1) / 2;
            std::string h = g.op("Relu", {conv(x, f + ".conv_1", pad)});
            h = conv(h, f + ".conv_2", pad);
            x = layerNorm(g.op("Add", {x, h}), "enc_p.encoder.norm_layers_2." + std::to_string(l));
        }
        x = newest(x, T, Tf, 2);
        const std::string stats = conv(x, "enc_p.proj", 0); // [1, 2*inter, Tf]
        const auto ml = split2(stats, inter, inter, 1);
        // z_p = m + exp(logs) * noise * 0.66666
        std::string z = g.op("Add", {ml[0], g.op("Mul", {g.op("Mul", {g.op("Exp", {ml[1]}), rnd}), g.scalar(0.66666f)})});

        // ------------------------------------------------- flow, backwards
        const int64_t half = inter / 2;
        for (int fi = kFlows - 1; fi >= 0; --fi) {
            // Flip (channel order), then the coupling layer undone.
            z = g.op("Slice", {z, g.ints({-1}), g.ints({-std::numeric_limits<int64_t>::max()}), g.ints({1}), g.ints({-1})});
            const std::string p = "flow.flows." + std::to_string(fi * 2);
            const auto parts = split2(z, half, half, 1);
            std::string hcur = conv(parts[0], p + ".pre", 0);
            const std::vector<float> cond = applyConv1x1(p + ".enc.cond_layer", spk); // [2*hidden*layers]
            std::string skip;
            for (int l = 0; l < kFlowLayers; ++l) {
                const std::vector<float> gl(cond.begin() + std::min<size_t>(cond.size(), static_cast<size_t>(l * 2 * C)),
                                            cond.begin() + std::min<size_t>(cond.size(), static_cast<size_t>((l + 1) * 2 * C)));
                const std::string xin = conv(hcur, p + ".enc.in_layers." + std::to_string(l), (kFlowKernel - 1) / 2, 1, 1, true, &gl);
                const auto ts = split2(xin, C, C, 1);
                const std::string acts = g.op("Mul", {g.op("Tanh", {ts[0]}), g.op("Sigmoid", {ts[1]})});
                const std::string rs = conv(acts, p + ".enc.res_skip_layers." + std::to_string(l), 0);
                if (l < kFlowLayers - 1) {
                    const auto rsk = split2(rs, C, C, 1);
                    hcur = g.op("Add", {hcur, rsk[0]});
                    skip = skip.empty() ? rsk[1] : g.op("Add", {skip, rsk[1]});
                } else {
                    skip = skip.empty() ? rs : g.op("Add", {skip, rs});
                }
            }
            const std::string m = conv(skip, p + ".post", 0);
            z = g.op("Concat", {parts[0], g.op("Sub", {parts[1], m})}, {attrInt("axis", 1)});
        }
        z = newest(z, Tf, D, 2);

        // ---------------------------------------------------------- vocoder
        // Source: a sine at the pitch (phase kept continuous across frames,
        // from the start of the block, so the newest frames get the phase they
        // would have had in a full run) plus noise; noise only where unvoiced.
        std::vector<float> grid(static_cast<size_t>(upp));
        for (int64_t k = 0; k < upp; ++k) grid[static_cast<size_t>(k)] = static_cast<float>(k + 1);
        const std::string f0 = g.op("Unsqueeze", {pitchf, g.ints({2})}); // [1,T,1]
        std::string ph = g.op("Mul", {g.op("Div", {f0, g.scalar(static_cast<float>(I.sampleRate))}), g.constant({1, 1, upp}, grid)});
        const std::string lastCol = g.op("Slice", {ph, g.ints({0, upp - 1}), g.ints({T - 1, upp}), g.ints({1, 2})}); // [1,T-1,1]
        const std::string rem =
            g.op("Sub", {g.op("Mod", {g.op("Add", {lastCol, g.scalar(0.5f)}), g.scalar(1.0f)}, {attrInt("fmod", 1)}), g.scalar(0.5f)});
        const std::string cum = g.op("Mod", {g.op("CumSum", {rem, g.constantInt64({}, {1})}), g.scalar(1.0f)}, {attrInt("fmod", 1)});
        ph = g.op("Add", {ph, g.op("Pad", {cum, g.ints({0, 1, 0, 0, 0, 0})})});
        ph = reshape(newest(ph, T, D, 1), {1, D * upp}); // the newest frames
        const std::string sine =
            g.op("Mul", {g.op("Sin", {g.op("Mul", {ph, g.scalar(2.0f * static_cast<float>(3.14159265358979323846))})}), g.scalar(0.1f)});
        std::string uv = g.op("Cast", {g.op("Greater", {newest(f0, T, D, 1), g.scalar(0.0f)})}, {attrInt("to", 1)}); // [1,D,1]
        uv = reshape(g.op("Expand", {uv, g.ints({1, D, upp})}), {1, D * upp});
        const std::string amp =
            g.op("Add", {g.op("Mul", {uv, g.scalar(0.003f)}), g.op("Mul", {g.op("Sub", {g.scalar(1.0f), uv}), g.scalar(0.1f / 3.0f)})});
        const std::string src = g.op("Add", {g.op("Mul", {sine, uv}), g.op("Mul", {amp, noise})});
        const W lw = need("dec.m_source.l_linear.weight"), lb = need("dec.m_source.l_linear.bias");
        std::string har = g.op("Tanh", {g.op("Add", {g.op("Mul", {src, g.scalar(lw.v.empty() ? 0.0f : lw.v[0])}),
                                                      g.scalar(lb.v.empty() ? 0.0f : lb.v[0])})});
        har = reshape(har, {1, 1, D * upp});

        const std::vector<float> condG = applyConv1x1("dec.cond", spk);
        std::string y = conv(z, "dec.conv_pre", 3, 1, 1, true, &condG);
        const int nk = static_cast<int>(I.rbKernels.size());
        for (size_t s = 0; s < I.upRates.size(); ++s) {
            y = lrelu(y, kLrelu);
            const int u = I.upRates[s], k = I.upKernels[s];
            const int pad = u % 2 == 0 ? (k - u) / 2 : u / 2 + u % 2;
            const std::string up = "dec.ups." + std::to_string(s);
            const W uw = need(up + ".weight"), ub = need(up + ".bias");
            if (u % 2 == 0) {
                y = g.op("ConvTranspose", {y, constant(uw), constant(ub)},
                         {attrInts("kernel_shape", {k}), attrInts("strides", {u}), attrInts("pads", {pad, pad})});
            } else {
                // Odd rates need output padding, which DirectML only accepts
                // in 2-D: the same transposed convolution with a height of 1.
                W uw2 = uw;
                uw2.shape = {uw.shape[0], uw.shape[1], 1, uw.shape[2]};
                const std::string y2 = g.op("ConvTranspose", {g.op("Unsqueeze", {y, g.ints({2})}), constant(uw2), constant(ub)},
                                            {attrInts("kernel_shape", {1, k}), attrInts("strides", {1, u}),
                                             attrInts("pads", {0, pad, 0, pad}), attrInts("output_padding", {0, 1})});
                y = g.op("Squeeze", {y2, g.ints({2})});
            }
            int strideF0 = 1;
            for (size_t r = s + 1; r < I.upRates.size(); ++r) strideF0 *= I.upRates[r];
            const int nkern = strideF0 == 1 ? 1 : strideF0 * 2 - strideF0 % 2;
            const int npad = strideF0 == 1 ? 0 : (nkern - strideF0) / 2;
            y = g.op("Add", {y, conv(har, "dec.noise_convs." + std::to_string(s), npad, 1, strideF0)});
            std::string sum;
            for (int r = 0; r < nk; ++r) {
                const std::string rb = "dec.resblocks." + std::to_string(s * nk + r);
                const int kk = I.rbKernels[r];
                std::string xr = y;
                for (size_t d = 0; d < I.rbDilations[r].size(); ++d) {
                    const int dil = I.rbDilations[r][d];
                    std::string t = conv(lrelu(xr, kLrelu), rb + ".convs1." + std::to_string(d), (kk * dil - dil) / 2, dil);
                    t = conv(lrelu(t, kLrelu), rb + ".convs2." + std::to_string(d), (kk - 1) / 2);
                    xr = g.op("Add", {t, xr});
                }
                sum = sum.empty() ? xr : g.op("Add", {sum, xr});
            }
            y = g.op("Div", {sum, g.scalar(static_cast<float>(nk))});
        }
        y = lrelu(y, 0.01f); // torch's default slope here
        y = g.op("Tanh", {conv(y, "dec.conv_post", 3, 1, 1, false)});
        g.output(reshape(y, {1, D * upp}), "audio", OnnxType::Float, {1, D * upp});
    }
    if (!missing.empty()) {
        if (error) *error = "The model is missing '" + missing + "'";
        return false;
    }
    out = g.serialize();
    return true;
}

} // namespace echo::ai
