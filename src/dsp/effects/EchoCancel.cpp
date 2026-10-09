// Acoustic echo cancellation (AEC).
//
// Removes the sound of your own speakers picked up by the microphone, using
// what is being played as a reference (normally the loopback of the
// playback device). This is a different problem from noise suppression (which
// removes noise that has no reference) and from gating (which mutes quiet
// passages).
//
// Linear stage: partitioned-block frequency-domain adaptive filter (MDF,
// Soo & Pang 1990) with overlap-save, per-bin power-normalised step size,
// round-robin gradient constraint, double-talk-aware step control and
// divergence protection.
// Non-linear stage: residual echo suppressor (spectral gain from the echo
// estimate vs. the error, on a 75%-overlap STFT).
#include "dsp/Fft.h"
#include "dsp/Stft.h"
#include "dsp/effects/EffectUtil.h"
#include "dsp/NoiseSuppressorInfo.h"

#include <complex>
#include <cstring>
#include <vector>

namespace echo {

namespace {

const ParamInfo kAecParams[] = {
    {"tail", "Echo tail", 50.0f, 500.0f, 250.0f, "ms", ParamScale::Linear, nullptr,
     "Longest echo delay + reverberation to cancel (playback/mic offset plus room)."},
    {"suppression", "Residual suppression", 0.0f, 100.0f, 60.0f, "%", ParamScale::Linear, nullptr,
     "Removes echo left over after the adaptive filter. Higher = drier, may affect double talk."},
    {"rate", "Adaptation speed", 10.0f, 100.0f, 50.0f, "%", ParamScale::Linear, nullptr,
     "How fast the filter learns the echo path."},
};
ECHO_EFFECT_INFO(kAecInfo, "echo_cancel", "Echo Cancellation (AEC)", "Restoration",
                 "Removes your speakers' sound from the microphone using the playback signal as a "
                 "reference. Needed with speakers, not with headphones.",
                 kAecParams, false);

using cfloat = std::complex<float>;

class EchoCancelEffect final : public Effect {
public:
    EchoCancelEffect() : Effect(kAecInfo) {}
    bool usesReference() const override { return true; }
    int latencySamples() const override { return latency_; }
    const char* visLabel(int s) const override {
        switch (s) {
        case 0: return "Echo reduction (ERLE dB)";
        case 1: return "Double talk";
        case 2: return "Reference level (dBFS)";
        case 3: return "Reference connected";
        default: return nullptr;
        }
    }

protected:
    void onPrepare() override {
        B_ = sr_ > 70000 ? 512 : 256;
        N_ = 2 * B_;
        K_ = B_ + 1;
        fft_.prepare(N_);
        pMax_ = static_cast<int>(std::ceil(0.5 * sr_ / B_)) + 1; // 500 ms
        for (auto& c : ch_) {
            c.W.assign(static_cast<size_t>(pMax_) * K_, cfloat(0.0f, 0.0f));
            c.micIn.assign(B_, 0.0f);
            c.out.assign(static_cast<size_t>(B_) * 4, 0.0f);
            c.stE.prepare(2 * B_);
            c.stY.prepare(2 * B_);
            c.yBlock.assign(B_, 0.0f);
            c.eBlock.assign(B_, 0.0f);
            c.Ymag.assign(B_ + 1, 0.0f);
            c.gain.assign(B_ + 1, 1.0f);
        }
        X_.assign(static_cast<size_t>(pMax_) * K_, cfloat(0.0f, 0.0f));
        Sx_.assign(K_, 0.0f);
        refBlock_.assign(B_, 0.0f);
        refPrev_.assign(B_, 0.0f);
        refIn_.assign(B_, 0.0f);
        time_.assign(N_, 0.0f);
        spec_.assign(static_cast<size_t>(N_) + 2, 0.0f);
        tmp_.assign(static_cast<size_t>(N_) + 2, 0.0f);
        latency_ = B_ + ch_[0].stE.latency();
    }

    void onReset() override {
        for (auto& c : ch_) {
            std::fill(c.W.begin(), c.W.end(), cfloat(0.0f, 0.0f));
            std::fill(c.out.begin(), c.out.end(), 0.0f);
            c.stE.reset();
            c.stY.reset();
            std::fill(c.gain.begin(), c.gain.end(), 1.0f);
            c.pd = c.pe = c.py = 1e-9f;
            c.outR = 0;
            c.outW = B_; // prime the output with one block
        }
        std::fill(X_.begin(), X_.end(), cfloat(0.0f, 0.0f));
        std::fill(Sx_.begin(), Sx_.end(), 0.0f);
        std::fill(refPrev_.begin(), refPrev_.end(), 0.0f);
        xHead_ = 0;
        fill_ = 0;
        px_ = 1e-9f;
        constrainNext_ = 0;
    }

    void processImpl(float* const* ch, int numCh, int n) override {
        const bool haveRef = refL_ != nullptr;
        vis[3].store(haveRef ? 1.0f : 0.0f, std::memory_order_relaxed);
        int pos = 0;
        while (pos < n) {
            const int take = std::min(n - pos, B_ - fill_);
            for (int c = 0; c < numCh; ++c) std::memcpy(ch_[c].micIn.data() + fill_, ch[c] + pos, sizeof(float) * take);
            for (int i = 0; i < take; ++i) {
                float r = 0.0f;
                if (haveRef) r = refCh_ > 1 ? 0.5f * (refL_[pos + i] + refR_[pos + i]) : refL_[pos + i];
                refIn_[fill_ + i] = r;
            }
            fill_ += take;
            if (fill_ == B_) {
                processBlock(numCh);
                fill_ = 0;
            }
            // Output (one block behind).
            for (int c = 0; c < numCh; ++c) {
                Chan& s = ch_[c];
                const int size = static_cast<int>(s.out.size());
                for (int i = 0; i < take; ++i) {
                    ch[c][pos + i] = s.out[s.outR];
                    s.outR = (s.outR + 1) % size;
                }
            }
            pos += take;
        }
    }

private:
    struct Chan {
        std::vector<cfloat> W;          // pMax_ x K_ filter partitions
        std::vector<float> micIn, out, yBlock, eBlock, Ymag, gain;
        Stft stE, stY;
        float pd = 1e-9f, pe = 1e-9f, py = 1e-9f;
        int outR = 0, outW = 0;
    };

    void processBlock(int numCh) {
        const int P = std::clamp(static_cast<int>(std::ceil(p(0) * 0.001 * sr_ / B_)), 1, pMax_);
        // Reference spectrum of [previous block | current block].
        std::memcpy(time_.data(), refPrev_.data(), sizeof(float) * B_);
        std::memcpy(time_.data() + B_, refIn_.data(), sizeof(float) * B_);
        std::memcpy(refPrev_.data(), refIn_.data(), sizeof(float) * B_);
        fft_.forward(time_.data(), spec_.data());
        xHead_ = (xHead_ + pMax_ - 1) % pMax_;
        cfloat* X0 = &X_[static_cast<size_t>(xHead_) * K_];
        double ex = 0.0;
        for (int k = 0; k < K_; ++k) X0[k] = cfloat(spec_[2 * k], spec_[2 * k + 1]);
        for (int i = 0; i < B_; ++i) ex += static_cast<double>(refIn_[i]) * refIn_[i];
        px_ = 0.8f * px_ + 0.2f * static_cast<float>(ex / B_);
        // Smoothed per-bin input power (times the number of partitions).
        for (int k = 0; k < K_; ++k) Sx_[k] = 0.9f * Sx_[k] + 0.1f * std::norm(X0[k]) * static_cast<float>(P);
        const bool refActive = px_ > 1e-8f; // about -80 dBFS
        vis[2].store(10.0f * std::log10(px_ + 1e-12f), std::memory_order_relaxed);

        const float mu0 = 0.05f + 0.45f * (p(2) / 100.0f);
        const float supp = p(1) / 100.0f;
        bool anyDt = false;
        float erleSum = 0.0f;
        for (int c = 0; c < numCh; ++c) {
            Chan& s = ch_[c];
            // Echo estimate Y = sum_p W_p X_p (overlap-save: last B samples).
            for (int k = 0; k < K_; ++k) {
                cfloat acc(0.0f, 0.0f);
                for (int p = 0; p < P; ++p) {
                    const int xi = (xHead_ + p) % pMax_;
                    acc += s.W[static_cast<size_t>(p) * K_ + k] * X_[static_cast<size_t>(xi) * K_ + k];
                }
                spec_[2 * k] = acc.real();
                spec_[2 * k + 1] = acc.imag();
            }
            fft_.inverse(spec_.data(), time_.data());
            double ed = 0.0, ee = 0.0, ey = 0.0;
            for (int i = 0; i < B_; ++i) {
                const float y = time_[B_ + i];
                const float d = s.micIn[i];
                const float e = d - y;
                s.yBlock[i] = y;
                s.eBlock[i] = e;
                ed += static_cast<double>(d) * d;
                ee += static_cast<double>(e) * e;
                ey += static_cast<double>(y) * y;
            }
            const float pd = static_cast<float>(ed / B_), pe = static_cast<float>(ee / B_), py = static_cast<float>(ey / B_);
            // Divergence protection: never output more than the microphone.
            if (pe > 2.0f * pd + 1e-9f) {
                for (auto& w : s.W) w *= 0.5f;
                std::memcpy(s.eBlock.data(), s.micIn.data(), sizeof(float) * B_);
                std::fill(s.yBlock.begin(), s.yBlock.end(), 0.0f);
            }
            s.pd = 0.9f * s.pd + 0.1f * pd;
            s.pe = 0.9f * s.pe + 0.1f * std::min(pe, pd);
            s.py = 0.9f * s.py + 0.1f * py;
            const float erle = 10.0f * std::log10((s.pd + 1e-12f) / (s.pe + 1e-12f));
            erleSum += erle;
            // Double talk: much more energy left than the filter explains,
            // once it has converged. Adaptation then nearly stops so the
            // near-end voice does not corrupt the echo path estimate.
            const bool doubleTalk = refActive && erle > 6.0f && s.pe > 4.0f * s.py;
            anyDt |= doubleTalk;
            float mu = refActive ? mu0 : 0.0f;
            if (doubleTalk) mu *= 0.03f;
            if (mu > 0.0f) {
                // Error spectrum of [0 | e].
                std::fill(time_.begin(), time_.begin() + B_, 0.0f);
                std::memcpy(time_.data() + B_, s.eBlock.data(), sizeof(float) * B_);
                fft_.forward(time_.data(), tmp_.data());
                for (int k = 0; k < K_; ++k) {
                    const cfloat E(tmp_[2 * k], tmp_[2 * k + 1]);
                    const float norm = mu / (Sx_[k] + 1e-6f * N_);
                    const cfloat g = E * norm;
                    for (int p = 0; p < P; ++p) {
                        const int xi = (xHead_ + p) % pMax_;
                        s.W[static_cast<size_t>(p) * K_ + k] += std::conj(X_[static_cast<size_t>(xi) * K_ + k]) * g;
                    }
                }
                // Gradient constraint on one partition per block (keeps the
                // filter a linear, not circular, convolution).
                constrain(s, constrainNext_ % P);
            }
            // Residual echo suppression on a 75%-overlap STFT of e and y.
            residualSuppress(s, refActive ? supp : 0.0f);
        }
        constrainNext_ = (constrainNext_ + 1) % std::max(1, P);
        vis[0].store(numCh ? erleSum / numCh : 0.0f, std::memory_order_relaxed);
        vis[1].store(anyDt ? 1.0f : 0.0f, std::memory_order_relaxed);
    }

    void constrain(Chan& s, int part) {
        cfloat* W = &s.W[static_cast<size_t>(part) * K_];
        for (int k = 0; k < K_; ++k) {
            tmp_[2 * k] = W[k].real();
            tmp_[2 * k + 1] = W[k].imag();
        }
        fft_.inverse(tmp_.data(), time_.data());
        std::fill(time_.begin() + B_, time_.end(), 0.0f);
        fft_.forward(time_.data(), tmp_.data());
        for (int k = 0; k < K_; ++k) W[k] = cfloat(tmp_[2 * k], tmp_[2 * k + 1]);
    }

    void residualSuppress(Chan& s, float supp) {
        const float over = 1.0f + 3.0f * supp;
        const float floorG = dbToGain(-40.0f * supp);
        const int hop = s.stE.hop();
        const int size = static_cast<int>(s.out.size());
        for (int off = 0; off < B_; off += hop) {
            float* yb = s.yBlock.data() + off;
            float* eb = s.eBlock.data() + off;
            // Analyse the echo estimate first so its magnitudes match the
            // error frame produced in the same hop.
            s.stY.process(yb, hop, [&](float* Y) {
                for (int k = 0; k <= B_; ++k) s.Ymag[k] = Y[2 * k] * Y[2 * k] + Y[2 * k + 1] * Y[2 * k + 1];
            });
            s.stE.process(eb, hop, [&](float* E) {
                for (int k = 0; k <= B_; ++k) {
                    const float pe = E[2 * k] * E[2 * k] + E[2 * k + 1] * E[2 * k + 1];
                    float g = supp > 0.0f ? std::max(floorG, 1.0f - over * s.Ymag[k] / (pe + 1e-12f)) : 1.0f;
                    g = std::max(g, s.gain[k] * 0.6f); // limit the release rate (fewer artifacts)
                    s.gain[k] = g;
                    E[2 * k] *= g;
                    E[2 * k + 1] *= g;
                }
            });
            for (int i = 0; i < hop; ++i) {
                s.out[s.outW] = eb[i];
                s.outW = (s.outW + 1) % size;
            }
        }
    }

    int B_ = 256, N_ = 512, K_ = 257, pMax_ = 48;
    RealFft fft_;
    Chan ch_[2];
    std::vector<cfloat> X_;    // pMax_ x K_ reference spectra (ring, newest at xHead_)
    std::vector<float> Sx_;    // smoothed reference power per bin
    std::vector<float> refBlock_, refPrev_, refIn_, time_, spec_, tmp_;
    int xHead_ = 0, fill_ = 0, constrainNext_ = 0, latency_ = 0;
    float px_ = 1e-9f;
};

} // namespace

const char* echoCancellerTypeId() { return kAecInfo.typeId; }

void registerEchoCancelEffects(EffectRegistry& r) { registerEffect<EchoCancelEffect>(r, kAecInfo); }

} // namespace echo
