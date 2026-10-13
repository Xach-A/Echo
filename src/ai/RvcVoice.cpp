#include "ai/RvcVoice.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <thread>

#include "ai/ContentVec.h"
#include "core/Log.h"
#include "core/Resampler.h"
#include "core/Text.h"
#include "session/Paths.h"

namespace echo::ai {

const char* const kContentVecUrl =
    "https://huggingface.co/IAHispano/Applio/resolve/main/Resources/embedders/contentvec/pytorch_model.bin";
const char* const kContentVecSha256 = "d8dd400e054ddf4e6be75dab5a2549db748cc99e756a097c496c099f65a4854e";

std::wstring contentVecPath() { return paths::appDataDir() + L"\\ai\\contentvec.bin"; }

bool loadRvcManifest(const std::wstring& dir, ModelManifest& m) {
    namespace fs = std::filesystem;
    std::error_code ec;
    m.kind = "rvc";
    m.dir = dir;
    m.id = wideToUtf8(fs::path(dir).filename().wstring());
    m.name = m.id;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file()) continue;
        std::wstring ext = e.path().extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
        if (ext == L".pth" && m.pthPath.empty()) m.pthPath = e.path().wstring();
        if (ext == L".index" && m.indexPath.empty()) m.indexPath = e.path().wstring();
    }
    if (m.pthPath.empty()) {
        m.error = "No voice found: put the RVC .pth file (and its .index, if it has one) in this folder";
        return false;
    }
    m.sizeBytes = fs::file_size(m.pthPath, ec);
    RvcModel rvc;
    std::string err;
    if (!rvc.load(m.pthPath, &err)) {
        m.error = err;
        return false;
    }
    const RvcInfo& i = rvc.info();
    if (!i.name.empty()) m.name = i.name;
    m.sampleRate = i.sampleRate;
    m.description = "RVC " + i.version + " voice, " + std::to_string(i.sampleRate / 1000) + " kHz" +
                    (m.indexPath.empty() ? "" : ", with index");
    m.license = "Not stated in RVC files: use only voices you have the right to use";
    // Blocks of 250 ms with 400 ms before them for context; the newest
    // 60 ms only blend into the next block (the networks see no future).
    // (Measured on an RVC v2 voice: closer to whole-file conversion than
    // shorter context, at about half of real time on a 12-core CPU.)
    m.chunkMs = 250;
    m.contextMs = 400;
    m.crossfadeMs = 60;
    m.valid = true;
    return true;
}

bool RvcVoice::load(const ModelManifest& m, bool preferGpu, int samples, std::string* error, const std::wstring& encoder) {
    const std::wstring cvPath = encoder.empty() ? contentVecPath() : encoder;
    RvcModel model;
    if (!model.load(m.pthPath, error)) return false;
    info_ = model.info();
    const int hop = info_.hop;
    // Whole 10 ms frames.
    frames_ = std::max(4, (samples + hop - 1) / hop);
    samples_ = frames_ * hop;
    n16_ = frames_ * 160;
    pad16_ = 480;
    featFrames_ = ContentVecModel::framesFor(n16_ + pad16_);
    if (featFrames_ * 2 < frames_) {
        if (error) *error = "Block too short";
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(cvPath, ec)) {
        if (error) *error = "RVC voices need the ContentVec speech model. Get it in Voices > AI voice.";
        return false;
    }
    ContentVecModel cv;
    std::vector<uint8_t> graph;
    const bool v1 = info_.version == "v1";
    if (!cv.load(cvPath, error) || !cv.buildGraph(n16_ + pad16_, v1 ? 9 : 0, v1, graph, error)) return false;
    if ((v1 ? 256 : cv.dim()) != info_.embDim) {
        if (error) *error = "The voice and the ContentVec model don't match";
        return false;
    }
    // Inference gets half the processor (the audio engine and the game keep the rest).
    const int threads = std::max(2, static_cast<int>(std::thread::hardware_concurrency()) / 2);
    if (!encoder_.create(graph, preferGpu, threads, error)) return false;
    graph.clear();
    graph.shrink_to_fit();
    if (!model.buildGenerator(frames_, 0, graph, error) || !voice_.create(graph, preferGpu, threads, error)) return false;
    provider_ = voice_.provider();
    if (!m.indexPath.empty()) {
        std::string e;
        if (!index_.load(m.indexPath, &e) || index_.dim() != info_.embDim) {
            ECHO_LOG_WARN("RVC index not used: %s", e.empty() ? "it doesn't match the voice" : e.c_str());
            index_ = FaissIndex{};
        }
    }
    x16_.assign(static_cast<size_t>(n16_ + pad16_), 0.0f);
    phone_.assign(static_cast<size_t>(frames_) * info_.embDim, 0.0f);
    pitch_.assign(static_cast<size_t>(frames_), 1);
    pitchf_.assign(static_cast<size_t>(frames_), 0.0f);
    rnd_.assign(static_cast<size_t>(info_.inter) * frames_, 0.0f);
    noise_.assign(static_cast<size_t>(samples_), 0.0f);
    ECHO_LOG_INFO("RVC voice '%s' ready: %s %d Hz, %d frames per block, %s, index %s", m.name.c_str(), info_.version.c_str(),
                  info_.sampleRate, frames_, provider_.c_str(), index_.loaded() ? "yes" : "no");
    return true;
}

void RvcVoice::trackPitch(const std::vector<float>& x16, int frames, std::vector<float>& f0) {
    PitchDetector det;
    det.prepare(16000.0, 50.0, 1100.0);
    det.setThresholds(0.15f, -50.0f);
    const int half = det.windowSamples() / 2;
    f0.assign(static_cast<size_t>(frames), 0.0f);
    size_t pos = 0;
    for (int i = 0; i < frames; ++i) {
        // Analyze when the window is centred on this frame.
        const size_t end = std::min(x16.size(), static_cast<size_t>(i * 160 + half));
        while (pos < end) det.push(x16[pos++]);
        if (static_cast<int>(pos) < det.windowSamples()) continue; // not enough audio yet
        const PitchDetector::Result r = det.analyze();
        f0[static_cast<size_t>(i)] = r.voiced ? r.hz : 0.0f;
    }
}

bool RvcVoice::run(const float* in, int n, float pitchSemis, std::vector<float>& out, std::string* error) {
    // The block at the network's size: newest audio at the end.
    std::vector<float> block(static_cast<size_t>(samples_), 0.0f);
    const int take = std::min(n, samples_);
    std::copy(in + (n - take), in + n, block.begin() + (samples_ - take));
    // 16 kHz for the speech encoder and the pitch, with a mirrored tail.
    std::vector<float> x16 = info_.sampleRate == 16000 ? block : resampleOffline(block, 1, info_.sampleRate, 16000.0);
    x16.resize(static_cast<size_t>(n16_), 0.0f);
    for (int k = 0; k < pad16_; ++k) x16.push_back(x16[static_cast<size_t>(std::max(0, n16_ - 2 - k))]);
    std::vector<std::vector<float>> res;
    const std::vector<OrtGraphSession::Input> encIn = {
        {"audio", OnnxType::Float, {1, n16_ + pad16_}, x16.data()}};
    if (!encoder_.run(encIn, {"features"}, res, error)) return false;
    feats_ = std::move(res[0]);
    const int D = info_.embDim;
    if (index_.loaded() && indexRate_ > 0.001f) {
        std::vector<float> near(static_cast<size_t>(D));
        for (int f = 0; f < featFrames_; ++f) {
            float* q = &feats_[static_cast<size_t>(f) * D];
            index_.retrieve(q, 8, near.data());
            for (int d = 0; d < D; ++d) q[d] = indexRate_ * near[static_cast<size_t>(d)] + (1.0f - indexRate_) * q[d];
        }
    }
    // Features to 100 a second (each twice), pitch per 10 ms.
    for (int i = 0; i < frames_; ++i) {
        const int f = std::min(i / 2, featFrames_ - 1);
        std::copy_n(&feats_[static_cast<size_t>(f) * D], D, &phone_[static_cast<size_t>(i) * D]);
    }
    trackPitch(x16, frames_, f0_);
    const float shift = std::pow(2.0f, pitchSemis / 12.0f);
    for (int i = 0; i < frames_; ++i) {
        const float hz = f0_[static_cast<size_t>(i)] * shift;
        pitchf_[static_cast<size_t>(i)] = hz;
        pitch_[static_cast<size_t>(i)] = rvcCoarsePitch(hz);
    }
    std::normal_distribution<float> nd(0.0f, 1.0f);
    for (float& v : rnd_) v = nd(rng_);
    for (float& v : noise_) v = nd(rng_);
    const std::vector<OrtGraphSession::Input> genIn = {
        {"phone", OnnxType::Float, {1, frames_, D}, phone_.data()},
        {"pitch", OnnxType::Int64, {1, frames_}, pitch_.data()},
        {"pitchf", OnnxType::Float, {1, frames_}, pitchf_.data()},
        {"rnd", OnnxType::Float, {1, info_.inter, frames_}, rnd_.data()},
        {"noise", OnnxType::Float, {1, samples_}, noise_.data()},
    };
    if (!voice_.run(genIn, {"audio"}, res, error)) return false;
    // Back to the caller's length (newest samples at the end).
    out.assign(static_cast<size_t>(n), 0.0f);
    const std::vector<float>& y = res[0];
    for (int k = 0; k < n && k < samples_; ++k) out[static_cast<size_t>(n - 1 - k)] = y[y.size() - 1 - static_cast<size_t>(k)];
    return true;
}

} // namespace echo::ai
