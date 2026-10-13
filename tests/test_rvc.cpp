// RVC voices: reading PyTorch checkpoints, the voice network and ContentVec
// built as ONNX graphs (checked against the official implementations' output
// on tiny random models, tests/data/rvc, made by tools/make_rvc_test_data.py),
// FAISS index retrieval, and the real-time pipeline.
#include "doctest/doctest.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "TestUtil.h"
#include "ai/ContentVec.h"
#include "ai/FaissIndex.h"
#include "ai/OnnxGraph.h"
#include "ai/OrtSession.h"
#include "ai/RvcModel.h"
#include "ai/RvcVoice.h"
#include "ai/TorchFile.h"
#include "audio/Download.h"
#include "core/Text.h"

using namespace echo;
using namespace echo::test;

namespace {

std::wstring rvcData(const std::string& name) { return utf8ToWide(std::string(ECHO_TEST_DATA_DIR) + "/rvc/" + name); }

template <typename T>
std::vector<T> readBin(const std::string& name) {
    std::vector<T> v;
    FILE* f = _wfopen(rvcData(name).c_str(), L"rb");
    if (!f) return v;
    std::fseek(f, 0, SEEK_END);
    v.resize(static_cast<size_t>(std::ftell(f)) / sizeof(T));
    std::fseek(f, 0, SEEK_SET);
    const size_t got = std::fread(v.data(), sizeof(T), v.size(), f);
    std::fclose(f);
    v.resize(got);
    return v;
}

bool ortAvailable() {
    std::string err;
    if (ai::OrtRuntime::instance().ensureLoaded(&err)) return true;
    MESSAGE("SKIPPED: ONNX Runtime not installed next to the tests (" << err << ")");
    return false;
}

double maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b) {
    double m = 0.0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) m = std::max(m, static_cast<double>(std::fabs(a[i] - b[i])));
    return a.size() == b.size() ? m : 1e9;
}

} // namespace

TEST_CASE("rvc: a PyTorch checkpoint is read without running anything in it") {
    ai::TorchCheckpoint ck;
    std::string err;
    REQUIRE_MESSAGE(ck.open(rvcData("tiny-voice/tiny.pth"), &err), err);
    const ai::PyRef root = ck.root();
    REQUIRE(root);
    CHECK(root->get("version")->s == "v2");
    CHECK(root->get("sr")->number() == 32000.0);
    const ai::PyRef config = root->get("config");
    REQUIRE(config);
    CHECK(config->items.size() == 18);
    CHECK(config->items[12]->items.size() == 4); // upsample rates
    const auto tensors = ai::TorchCheckpoint::tensors(root->get("weight"));
    CHECK(tensors.size() > 100);
    const auto it = tensors.find("enc_p.emb_phone.weight");
    REQUIRE(it != tensors.end());
    CHECK(it->second.dtype == "float16");
    CHECK(it->second.shape == std::vector<int64_t>{16, 128});
    std::vector<float> w;
    REQUIRE(ck.read(it->second, w, &err));
    CHECK(w.size() == 16u * 128u);
    CHECK(allFinite(w));
    // Not a checkpoint: a clear message.
    ai::TorchCheckpoint bad;
    CHECK_FALSE(bad.open(rvcData("cv_audio.bin"), &err));
    CHECK(err.find("Not a PyTorch checkpoint") != std::string::npos);
}

TEST_CASE("rvc: the voice network matches the official implementation") {
    if (!ortAvailable()) return;
    ai::RvcModel model;
    std::string err;
    REQUIRE_MESSAGE(model.load(rvcData("tiny-voice/tiny.pth"), &err), err);
    const ai::RvcInfo& i = model.info();
    CHECK(i.sampleRate == 32000);
    CHECK(i.hop == 320);
    CHECK(i.embDim == 128);
    const int T = 40;
    std::vector<uint8_t> onnx;
    REQUIRE_MESSAGE(model.buildGenerator(T, 0, onnx, &err), err);
    ai::OrtGraphSession s;
    REQUIRE_MESSAGE(s.create(onnx, false, 2, &err), err);
    const auto phone = readBin<float>("gen_phone.bin"), pitchf = readBin<float>("gen_pitchf.bin"), rnd = readBin<float>("gen_rnd.bin"),
               noise = readBin<float>("gen_noise.bin"), expected = readBin<float>("gen_audio.bin");
    const auto pitch = readBin<int64_t>("gen_pitch.bin");
    std::vector<std::vector<float>> out;
    REQUIRE_MESSAGE(s.run({{"phone", ai::OnnxType::Float, {1, T, 128}, phone.data()},
                           {"pitch", ai::OnnxType::Int64, {1, T}, pitch.data()},
                           {"pitchf", ai::OnnxType::Float, {1, T}, pitchf.data()},
                           {"rnd", ai::OnnxType::Float, {1, 16, T}, rnd.data()},
                           {"noise", ai::OnnxType::Float, {1, T * 320}, noise.data()}},
                          {"audio"}, out, &err),
                    err);
    double rmsRef = 0.0;
    for (float v : expected) rmsRef += v * v;
    rmsRef = std::sqrt(rmsRef / std::max<size_t>(1, expected.size()));
    const double err1 = maxAbsDiff(out[0], expected);
    MESSAGE("voice network: " << out[0].size() << " samples, max difference " << err1 << " (signal rms " << rmsRef << ")");
    CHECK(out[0].size() == expected.size());
    CHECK(rmsRef > 1e-3);
    CHECK(err1 < 1e-4);
}

TEST_CASE("rvc: ContentVec matches the official implementation") {
    if (!ortAvailable()) return;
    ai::ContentVecModel cv;
    std::string err;
    REQUIRE_MESSAGE(cv.load(rvcData("contentvec-tiny.bin"), &err), err);
    CHECK(cv.dim() == 128);
    CHECK(cv.layers() == 2);
    const auto audio = readBin<float>("cv_audio.bin"), expected = readBin<float>("cv_feats.bin");
    const int n = static_cast<int>(audio.size());
    CHECK(ai::ContentVecModel::framesFor(n) == static_cast<int>(expected.size() / 128));
    std::vector<uint8_t> onnx;
    REQUIRE_MESSAGE(cv.buildGraph(n, 0, false, onnx, &err), err);
    ai::OrtGraphSession s;
    REQUIRE_MESSAGE(s.create(onnx, false, 2, &err), err);
    std::vector<std::vector<float>> out;
    REQUIRE_MESSAGE(s.run({{"audio", ai::OnnxType::Float, {1, n}, audio.data()}}, {"features"}, out, &err), err);
    const double d = maxAbsDiff(out[0], expected);
    MESSAGE("ContentVec: " << expected.size() / 128 << " frames, max difference " << d);
    CHECK(d < 1e-3);
}

TEST_CASE("rvc: on the graphics card (DirectML) the voice network and ContentVec give the same result") {
    if (!ortAvailable()) return;
    if (!ai::OrtRuntime::instance().hasGpu()) {
        MESSAGE("SKIPPED: this ONNX Runtime has no DirectML (the CPU build)");
        return;
    }
    std::string err;
    // The voice network.
    ai::RvcModel model;
    REQUIRE_MESSAGE(model.load(rvcData("tiny-voice/tiny.pth"), &err), err);
    const int T = 40;
    std::vector<uint8_t> onnx;
    REQUIRE_MESSAGE(model.buildGenerator(T, 0, onnx, &err), err);
    ai::OrtGraphSession s;
    REQUIRE_MESSAGE(s.create(onnx, true, 2, &err), err);
    MESSAGE("running on " << s.provider());
    CHECK(s.provider().rfind("DirectML", 0) == 0);
    const auto phone = readBin<float>("gen_phone.bin"), pitchf = readBin<float>("gen_pitchf.bin"), rnd = readBin<float>("gen_rnd.bin"),
               noise = readBin<float>("gen_noise.bin"), expected = readBin<float>("gen_audio.bin");
    const auto pitch = readBin<int64_t>("gen_pitch.bin");
    std::vector<std::vector<float>> out;
    REQUIRE_MESSAGE(s.run({{"phone", ai::OnnxType::Float, {1, T, 128}, phone.data()},
                           {"pitch", ai::OnnxType::Int64, {1, T}, pitch.data()},
                           {"pitchf", ai::OnnxType::Float, {1, T}, pitchf.data()},
                           {"rnd", ai::OnnxType::Float, {1, 16, T}, rnd.data()},
                           {"noise", ai::OnnxType::Float, {1, T * 320}, noise.data()}},
                          {"audio"}, out, &err),
                    err);
    const double d1 = maxAbsDiff(out[0], expected);
    MESSAGE("voice network on the graphics card: max difference " << d1);
    CHECK(d1 < 1e-3);
    // ContentVec.
    ai::ContentVecModel cv;
    REQUIRE_MESSAGE(cv.load(rvcData("contentvec-tiny.bin"), &err), err);
    const auto audio = readBin<float>("cv_audio.bin"), feats = readBin<float>("cv_feats.bin");
    const int n = static_cast<int>(audio.size());
    std::vector<uint8_t> cvOnnx;
    REQUIRE_MESSAGE(cv.buildGraph(n, 0, false, cvOnnx, &err), err);
    ai::OrtGraphSession c;
    REQUIRE_MESSAGE(c.create(cvOnnx, true, 2, &err), err);
    REQUIRE_MESSAGE(c.run({{"audio", ai::OnnxType::Float, {1, n}, audio.data()}}, {"features"}, out, &err), err);
    const double d2 = maxAbsDiff(out[0], feats);
    MESSAGE("ContentVec on the graphics card: max difference " << d2);
    CHECK(d2 < 1e-2);
}

TEST_CASE("rvc: an odd upsampling rate built in 2-D (for DirectML) equals the 1-D transposed convolution") {
    if (!ortAvailable()) return;
    // Rate 3, kernel 7: the 1-D form with output padding vs the 2-D form the
    // voice network uses for odd rates.
    const int cin = 4, cout = 3, L = 25, k = 7, u = 3, pad = u / 2 + u % 2;
    std::vector<float> w(static_cast<size_t>(cin * cout * k)), b(cout), x(static_cast<size_t>(cin * L));
    for (size_t i = 0; i < w.size(); ++i) w[i] = std::sin(0.37f * static_cast<float>(i));
    for (size_t i = 0; i < b.size(); ++i) b[i] = 0.1f * static_cast<float>(i);
    for (size_t i = 0; i < x.size(); ++i) x[i] = std::cos(0.11f * static_cast<float>(i));
    const int outLen = (L - 1) * u - 2 * pad + k + 1;
    auto run = [&](bool twoD) {
        ai::OnnxGraph g;
        g.input("x", ai::OnnxType::Float, {1, cin, L});
        std::string y;
        if (!twoD) {
            y = g.op("ConvTranspose", {"x", g.constant({cin, cout, k}, w), g.constant({cout}, b)},
                     {ai::attrInts("kernel_shape", {k}), ai::attrInts("strides", {u}), ai::attrInts("pads", {pad, pad}),
                      ai::attrInts("output_padding", {1})});
        } else {
            const std::string y2 = g.op("ConvTranspose", {g.op("Unsqueeze", {"x", g.ints({2})}), g.constant({cin, cout, 1, k}, w),
                                                          g.constant({cout}, b)},
                                        {ai::attrInts("kernel_shape", {1, k}), ai::attrInts("strides", {1, u}),
                                         ai::attrInts("pads", {0, pad, 0, pad}), ai::attrInts("output_padding", {0, 1})});
            y = g.op("Squeeze", {y2, g.ints({2})});
        }
        g.output(y, "y", ai::OnnxType::Float, {1, cout, outLen});
        ai::OrtGraphSession s;
        std::string err;
        std::vector<std::vector<float>> out;
        REQUIRE_MESSAGE(s.create(g.serialize(), false, 1, &err), err);
        REQUIRE_MESSAGE(s.run({{"x", ai::OnnxType::Float, {1, cin, L}, x.data()}}, {"y"}, out, &err), err);
        return out[0];
    };
    const std::vector<float> a = run(false), c = run(true);
    CHECK(a.size() == static_cast<size_t>(cout * outLen));
    CHECK(maxAbsDiff(a, c) < 1e-6);
}

TEST_CASE("rvc: index retrieval finds the voice's nearest features like RVC does") {
    ai::FaissIndex idx;
    std::string err;
    REQUIRE_MESSAGE(idx.load(rvcData("tiny-voice/tiny.index"), &err), err);
    CHECK(idx.dim() == 128);
    CHECK(idx.size() == 100);
    const auto q = readBin<float>("index_queries.bin"), expected = readBin<float>("index_expected.bin");
    std::vector<float> out(128);
    for (int k = 0; k < 2; ++k) {
        idx.retrieve(&q[static_cast<size_t>(k) * 128], 8, out.data());
        const std::vector<float> e(expected.begin() + k * 128, expected.begin() + (k + 1) * 128);
        CHECK(maxAbsDiff(out, e) < 1e-4);
    }
    // Not an index: refused with a message.
    ai::FaissIndex bad;
    CHECK_FALSE(bad.load(rvcData("cv_audio.bin"), &err));
    CHECK_FALSE(err.empty());
}

TEST_CASE("rvc: coarse pitch follows RVC's mel scale") {
    CHECK(ai::rvcCoarsePitch(0.0f) == 1);
    CHECK(ai::rvcCoarsePitch(50.0f) == 1);
    CHECK(ai::rvcCoarsePitch(1100.0f) == 255);
    CHECK(ai::rvcCoarsePitch(5000.0f) == 255);
    const int64_t mid = ai::rvcCoarsePitch(200.0f);
    CHECK(mid > 40);
    CHECK(mid < 120);
    CHECK(ai::rvcCoarsePitch(220.0f) > mid);
}

TEST_CASE("rvc: voice folders: an RVC voice is found, a folder without one says what to do") {
    ai::ModelManifest m;
    REQUIRE(ai::loadManifest(rvcData("tiny-voice"), m));
    CHECK(m.kind == "rvc");
    CHECK(m.valid);
    CHECK(m.name == "Tiny test voice");
    CHECK(m.sampleRate == 32000);
    CHECK_FALSE(m.indexPath.empty());
    CHECK(m.description.find("with index") != std::string::npos);
    // An empty folder.
    const std::filesystem::path empty = std::filesystem::temp_directory_path() / "echo-test-empty-voice";
    std::error_code ec;
    std::filesystem::create_directories(empty, ec);
    ai::ModelManifest none;
    CHECK_FALSE(ai::loadManifest(empty.wstring(), none));
    CHECK(none.error.find(".pth") != std::string::npos);
    std::filesystem::remove_all(empty, ec);
}

TEST_CASE("rvc: the real-time pipeline converts blocks of voice") {
    if (!ortAvailable()) return;
    ai::ModelManifest m;
    REQUIRE(ai::loadManifest(rvcData("tiny-voice"), m));
    ai::RvcVoice voice;
    std::string err;
    const int block = 32000 * 650 / 1000; // 400 ms context + 250 ms
    REQUIRE_MESSAGE(voice.load(m, false, block, &err, rvcData("contentvec-tiny.bin")), err);
    CHECK(voice.hasIndex());
    // A voiced sound at 150 Hz.
    std::vector<float> in(static_cast<size_t>(block));
    for (size_t i = 0; i < in.size(); ++i)
        in[i] = 0.2f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 150.0 * i / 32000.0) +
                                          0.5 * std::sin(2.0 * 3.14159265358979 * 300.0 * i / 32000.0));
    std::vector<float> out;
    voice.setIndexRate(0.5f);
    REQUIRE_MESSAGE(voice.run(in.data(), block, 0.0f, out, &err), err);
    CHECK(out.size() == in.size());
    CHECK(allFinite(out));
    CHECK(rms(out, 0, out.size()) > 1e-4f);
    // A block one sample short (the resampler varies) works too.
    REQUIRE(voice.run(in.data(), block - 1, 12.0f, out, &err));
    CHECK(out.size() == in.size() - 1);
    // The pitch tracker finds 150 Hz.
    std::vector<float> x16(8000), f0;
    for (size_t i = 0; i < x16.size(); ++i) x16[i] = 0.3f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 150.0 * i / 16000.0));
    ai::RvcVoice::trackPitch(x16, 50, f0);
    CHECK(f0[30] == doctest::Approx(150.0).epsilon(0.02));
}

TEST_CASE("rvc: ContentVec is found where Echo keeps it, and checked by its SHA-256") {
    CHECK(ai::contentVecPath().find(L"\\ai\\contentvec.bin") != std::wstring::npos);
    CHECK(std::string(ai::kContentVecUrl).rfind("https://huggingface.co/IAHispano/Applio/", 0) == 0);
    // SHA-256 of a known file ("abc" -> the standard test vector).
    const std::filesystem::path f = std::filesystem::temp_directory_path() / "echo-test-sha.txt";
    FILE* fp = _wfopen(f.wstring().c_str(), L"wb");
    REQUIRE(fp);
    std::fwrite("abc", 1, 3, fp);
    std::fclose(fp);
    CHECK(win::sha256File(f.wstring()) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    std::error_code ec;
    std::filesystem::remove(f, ec);
}
