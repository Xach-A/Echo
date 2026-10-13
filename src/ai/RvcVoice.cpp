#include "ai/RvcVoice.h"

#include <algorithm>
#include <chrono>
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

namespace {

std::wstring lowered(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

// The .index that fits a voice best: in the voice's own folder, then with
// the voice's name in its own, then RVC's "added_" (searchable) indexes.
// "trained_" indexes hold no voice features and are never used, and neither
// is an index that belongs to another voice found (in its folder or named
// after it).
std::wstring bestIndex(const std::filesystem::path& pth, const std::vector<std::filesystem::path>& indexes,
                       const std::vector<std::filesystem::path>& allPths) {
    const std::wstring stem = lowered(pth.stem().wstring());
    int best = -1;
    std::wstring out;
    for (const auto& ix : indexes) {
        const std::wstring name = lowered(ix.filename().wstring());
        if (name.rfind(L"trained", 0) == 0) continue;
        const bool mine = ix.parent_path() == pth.parent_path() || (!stem.empty() && name.find(stem) != std::wstring::npos);
        bool other = false;
        for (const auto& o : allPths) {
            if (o == pth) continue;
            const std::wstring os = lowered(o.stem().wstring());
            other |= ix.parent_path() == o.parent_path() || (!os.empty() && name.find(os) != std::wstring::npos);
        }
        if (other && !mine) continue;
        int score = 0;
        if (ix.parent_path() == pth.parent_path()) score += 4;
        if (!stem.empty() && name.find(stem) != std::wstring::npos) score += 2;
        if (name.rfind(L"added", 0) == 0) score += 1;
        if (score > best) {
            best = score;
            out = ix.wstring();
        }
    }
    return out;
}

} // namespace

std::vector<ModelManifest> findRvcVoices(const std::wstring& dir, bool subfolders) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<fs::path> pths, indexes;
    auto consider = [&](const fs::path& p) {
        const std::wstring ext = lowered(p.extension().wstring());
        if (ext == L".pth") pths.push_back(p);
        else if (ext == L".index") indexes.push_back(p);
    };
    if (subfolders) {
        // Four folders deep at most, and not more than 5000 entries.
        int seen = 0;
        for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator() && seen < 5000; it.increment(ec), ++seen) {
            if (it->is_directory(ec) && it.depth() >= 3) it.disable_recursion_pending();
            else if (it->is_regular_file(ec)) consider(it->path());
        }
    } else {
        for (const auto& e : fs::directory_iterator(dir, ec))
            if (e.is_regular_file(ec)) consider(e.path());
    }
    // Files nearer the top first, then by path (a stable order).
    auto depth = [&](const fs::path& p) {
        const fs::path rel = p.lexically_relative(dir);
        return std::distance(rel.begin(), rel.end());
    };
    std::sort(pths.begin(), pths.end(), [&](const fs::path& a, const fs::path& b) {
        const auto da = depth(a), db = depth(b);
        return da != db ? da < db : a < b;
    });
    // The playable voices (training checkpoints and other .pth files don't load).
    std::vector<std::pair<fs::path, RvcInfo>> found;
    for (const auto& pth : pths) {
        RvcModel rvc;
        std::string err;
        if (rvc.load(pth.wstring(), &err)) found.emplace_back(pth, rvc.info());
        if (found.size() >= 32) break;
    }
    std::vector<fs::path> voicePaths;
    for (const auto& f : found) voicePaths.push_back(f.first);
    std::vector<ModelManifest> out;
    for (const auto& [pth, i] : found) {
        ModelManifest m;
        m.kind = "rvc";
        m.dir = dir;
        m.id = wideToUtf8(pth.stem().wstring());
        m.pthPath = pth.wstring();
        m.indexPath = bestIndex(pth, indexes, voicePaths);
        m.sizeBytes = fs::file_size(pth, ec);
        m.name = i.name; // empty: the caller names it after its folder or file
        m.sampleRate = i.sampleRate;
        m.description = "RVC " + i.version + " voice, " + std::to_string(i.sampleRate / 1000) + " kHz" +
                        (m.indexPath.empty() ? "" : ", with index");
        m.license = "Not stated in RVC files: use only voices you have the right to use";
        // Blocks of 250 ms with 400 ms before them for context; the newest
        // 60 ms only blend into the next block (the networks see no future).
        // The AI voice's Response setting chooses the sizes it really uses.
        m.chunkMs = 250;
        m.contextMs = 400;
        m.crossfadeMs = 60;
        m.valid = true;
        out.push_back(std::move(m));
    }
    if (out.empty() && !pths.empty()) {
        ModelManifest m;
        m.kind = "rvc";
        m.dir = dir;
        m.error = "No playable voice here: the .pth files are not RVC voice models (G_ and D_ files are training "
                  "checkpoints, not voices)";
        out.push_back(std::move(m));
    }
    return out;
}

bool loadRvcManifest(const std::wstring& dir, ModelManifest& m) {
    const std::string folder = wideToUtf8(std::filesystem::path(dir).filename().wstring());
    std::vector<ModelManifest> voices = findRvcVoices(dir, true);
    if (voices.empty()) {
        m.kind = "rvc";
        m.dir = dir;
        m.error = "No voice found: put the RVC .pth file (and its .index, if it has one) in this folder";
    } else {
        m = std::move(voices.front());
    }
    m.id = folder;
    if (m.name.empty()) m.name = folder;
    return m.valid;
}

bool RvcVoice::load(const ModelManifest& m, bool preferGpu, int samples, std::string* error, const std::wstring& encoder,
                    int decodeSamples) {
    const std::wstring cvPath = encoder.empty() ? contentVecPath() : encoder;
    RvcModel model;
    if (!model.load(m.pthPath, error)) return false;
    info_ = model.info();
    const int hop = info_.hop;
    // Whole 10 ms frames.
    frames_ = std::max(4, (samples + hop - 1) / hop);
    samples_ = frames_ * hop;
    decFrames_ = decodeSamples > 0 ? std::min(frames_, (decodeSamples + hop - 1) / hop) : frames_;
    flowFrames_ = RvcModel::flowFrames(frames_, decFrames_);
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
    if (!model.buildGenerator(frames_, 0, graph, error, decFrames_ < frames_ ? decFrames_ : 0) ||
        !voice_.create(graph, preferGpu, threads, error))
        return false;
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
    rnd_.assign(static_cast<size_t>(info_.inter) * flowFrames_, 0.0f);
    noise_.assign(static_cast<size_t>(decFrames_) * hop, 0.0f);
    ECHO_LOG_INFO("RVC voice '%s' ready: %s %d Hz, %d frames per block (%d made), %s, index %s", m.name.c_str(),
                  info_.version.c_str(), info_.sampleRate, frames_, decFrames_, provider_.c_str(), index_.loaded() ? "yes" : "no");
    return true;
}

void RvcVoice::trackPitch(const std::vector<float>& x16, int frames, std::vector<float>& f0, int firstFrame) {
    PitchDetector det;
    det.prepare(16000.0, 50.0, 1100.0);
    det.setThresholds(0.15f, -50.0f);
    const int half = det.windowSamples() / 2;
    firstFrame = std::clamp(firstFrame, 0, frames);
    f0.resize(static_cast<size_t>(frames), 0.0f);
    // From three windows before the first frame: the detector's filter settles.
    size_t pos = static_cast<size_t>(std::max(0, firstFrame * 160 + half - 3 * det.windowSamples()));
    const size_t start = pos;
    for (int i = firstFrame; i < frames; ++i) {
        // Analyze when the window is centred on this frame.
        const size_t end = std::min(x16.size(), static_cast<size_t>(i * 160 + half));
        while (pos < end) det.push(x16[pos++]);
        float hz = 0.0f;
        if (static_cast<int>(pos - start) >= det.windowSamples()) { // enough audio
            const PitchDetector::Result r = det.analyze();
            if (r.voiced) hz = r.hz;
        }
        f0[static_cast<size_t>(i)] = hz;
    }
}

bool RvcVoice::run(const float* in, int n, float pitchSemis, std::vector<float>& out, std::string* error) {
    using clock = std::chrono::steady_clock;
    auto since = [](clock::time_point t) { return std::chrono::duration<double>(clock::now() - t).count(); };
    auto t = clock::now();
    // The block at the network's size: newest audio at the end.
    std::vector<float> block(static_cast<size_t>(samples_), 0.0f);
    const int take = std::min(n, samples_);
    std::copy(in + (n - take), in + n, block.begin() + (samples_ - take));
    // 16 kHz for the speech encoder and the pitch, with a mirrored tail.
    std::vector<float> x16 = info_.sampleRate == 16000 ? block : resampleOffline(block, 1, info_.sampleRate, 16000.0);
    x16.resize(static_cast<size_t>(n16_), 0.0f);
    for (int k = 0; k < pad16_; ++k) x16.push_back(x16[static_cast<size_t>(std::max(0, n16_ - 2 - k))]);
    times_.prep = since(t);
    t = clock::now();
    std::vector<std::vector<float>> res;
    const std::vector<OrtGraphSession::Input> encIn = {
        {"audio", OnnxType::Float, {1, n16_ + pad16_}, x16.data()}};
    if (!encoder_.run(encIn, {"features"}, res, error)) return false;
    feats_ = std::move(res[0]);
    times_.encoder = since(t);
    t = clock::now();
    const int D = info_.embDim;
    if (index_.loaded() && indexRate_ > 0.001f) {
        // Only where the flow runs (as RVC's real-time mode): the context
        // before it only informs the text encoder.
        const int from = std::min((frames_ - flowFrames_) / 2, featFrames_), count = featFrames_ - from;
        float* q = &feats_[static_cast<size_t>(from) * D];
        std::vector<float> near(static_cast<size_t>(count) * D);
        index_.retrieveMany(q, count, 8, near.data());
        for (size_t i = 0; i < near.size(); ++i) q[i] = indexRate_ * near[i] + (1.0f - indexRate_) * q[i];
    }
    times_.index = since(t);
    t = clock::now();
    // Features to 100 a second (each twice), pitch per 10 ms.
    for (int i = 0; i < frames_; ++i) {
        const int f = std::min(i / 2, featFrames_ - 1);
        std::copy_n(&feats_[static_cast<size_t>(f) * D], D, &phone_[static_cast<size_t>(i) * D]);
    }
    // The pitch: when this block is the last one moved on, its frames up to
    // the new audio are the last block's (moved along); only the end is new.
    int firstFrame = 0;
    if (advance_ > 0 && havePitch_) {
        const int shift = (advance_ + info_.hop / 2) / info_.hop;
        if (shift < frames_) {
            std::move(f0_.begin() + shift, f0_.end(), f0_.begin());
            firstFrame = std::max(0, frames_ - shift - kPitchReach);
        }
    }
    advance_ = 0;
    trackPitch(x16, frames_, f0_, firstFrame);
    havePitch_ = true;
    const float shift = std::pow(2.0f, pitchSemis / 12.0f);
    for (int i = 0; i < frames_; ++i) {
        const float hz = f0_[static_cast<size_t>(i)] * shift;
        pitchf_[static_cast<size_t>(i)] = hz;
        pitch_[static_cast<size_t>(i)] = rvcCoarsePitch(hz);
    }
    times_.pitch = since(t);
    t = clock::now();
    std::normal_distribution<float> nd(0.0f, 1.0f);
    for (float& v : rnd_) v = nd(rng_);
    for (float& v : noise_) v = nd(rng_);
    const std::vector<OrtGraphSession::Input> genIn = {
        {"phone", OnnxType::Float, {1, frames_, D}, phone_.data()},
        {"pitch", OnnxType::Int64, {1, frames_}, pitch_.data()},
        {"pitchf", OnnxType::Float, {1, frames_}, pitchf_.data()},
        {"rnd", OnnxType::Float, {1, info_.inter, flowFrames_}, rnd_.data()},
        {"noise", OnnxType::Float, {1, static_cast<int64_t>(noise_.size())}, noise_.data()},
    };
    if (!voice_.run(genIn, {"audio"}, res, error)) return false;
    times_.voice = since(t);
    // Back to the caller's length (newest samples at the end; the context
    // before the made audio stays silent).
    out.assign(static_cast<size_t>(n), 0.0f);
    const std::vector<float>& y = res[0];
    for (size_t k = 0; k < static_cast<size_t>(n) && k < y.size(); ++k) out[static_cast<size_t>(n) - 1 - k] = y[y.size() - 1 - k];
    return true;
}

} // namespace echo::ai
