// The soundboard: Echo's built-in sounds, the player (play / restart /
// toggle / hold / stop all, bank swaps), resampling and decoding sound files,
// routing into the virtual mic, and saving.
#include "doctest/doctest.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

#include "FakeDevices.h"
#include "TestUtil.h"
#include "audio/AudioDecoder.h"
#include "audio/WinUtil.h"
#include "core/Resampler.h"
#include "core/SoundboardSource.h"
#include "core/WavFile.h"
#include "session/MixerController.h"
#include "session/Paths.h"
#include "session/Serialization.h"
#include "session/Soundboard.h"

using namespace echo;
using namespace echo::test;

namespace {

std::shared_ptr<const SoundClip> toneClip(double freq, double seconds, float amp, int sr = 48000) {
    auto c = std::make_shared<SoundClip>();
    const size_t n = static_cast<size_t>(seconds * sr);
    c->samples.resize(n * 2);
    for (size_t i = 0; i < n; ++i) c->samples[2 * i] = c->samples[2 * i + 1] = amp * static_cast<float>(std::sin(2 * kPiD * freq * i / sr));
    return c;
}

// Pulls `blocks` blocks and returns the left channel.
std::vector<float> pullBlocks(SoundboardSource& s, int blocks) {
    std::vector<float> out;
    for (int b = 0; b < blocks; ++b) {
        s.pull(480);
        out.insert(out.end(), s.channel(0), s.channel(0) + 480);
    }
    return out;
}

float maxStep(const std::vector<float>& x) {
    float m = 0.0f;
    for (size_t i = 1; i < x.size(); ++i) m = std::max(m, std::fabs(x[i] - x[i - 1]));
    return m;
}

struct World {
    FakeBackend backend;
    std::unique_ptr<MixerController> mc;
    World() {
        backend.addDevice("mic", "USB Mic", false, 1, 48000, true);
        backend.addDevice("phones", "Headphones", true, 2, 48000, true);
        backend.addDevice("cable_in", "CABLE Input", true, 2, 48000, false, true);
        backend.addDevice("cable_out", "CABLE Output", false, 2, 48000, false, true);
        backend.cablePartners["cable_in"] = "cable_out";
        mc = std::make_unique<MixerController>(backend);
        mc->session().autoNoiseSuppression = false;
    }
    void step(int blocks) {
        for (int i = 0; i < blocks; ++i) {
            if (auto c = backend.captures["mic"]) c->produceSine(480, 440.0);
            mc->engine().processBlock();
            for (auto& [id, r] : backend.renders) r->consume(480);
        }
    }
};

double renderAmp(FakeRender& r, double freq, int ch = 0, int channels = 2, double seconds = 0.1) {
    const size_t frames = r.out.size() / channels;
    const size_t n = std::min(frames, static_cast<size_t>(seconds * 48000));
    std::vector<float> x(n);
    for (size_t i = 0; i < n; ++i) x[i] = r.out[(frames - n + i) * channels + ch];
    return toneAmplitude(x.data(), n, freq, 48000.0);
}

} // namespace

TEST_CASE("soundboard: every built-in sound is shipped, credited, loud enough, under -1 dBFS, and starts and ends quietly") {
    // The recordings next to the executable (copied there by the build).
    std::string credits;
    if (FILE* f = _wfopen((paths::builtinSoundsDir() + L"\\CREDITS.txt").c_str(), L"rb")) {
        char buf[4096];
        for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) credits.append(buf, n);
        fclose(f);
    }
    REQUIRE_MESSAGE(!credits.empty(), "sounds\\CREDITS.txt next to the tests");
    for (const auto& b : builtinSounds()) {
        WavInfo info;
        const std::string id = b.id;
        REQUIRE_MESSAGE(readWavInfo(builtinSoundFile(id), info), id);
        CHECK_MESSAGE(info.sampleRate == 48000, id);
        CHECK_MESSAGE(credits.find(id + ".wav") != std::string::npos, id); // source and license listed
    }
    for (int sr : {48000, 44100}) {
        for (const auto& b : builtinSounds()) {
            std::string err;
            const std::vector<float> x = loadBuiltinSound(b.id, sr, &err);
            REQUIRE_MESSAGE(!x.empty(), std::string(b.id) << ": " << err);
            const size_t frames = x.size() / 2;
            const double seconds = static_cast<double>(frames) / sr;
            float peak = 0.0f;
            double sum = 0.0;
            for (float v : x) {
                peak = std::max(peak, std::fabs(v));
                sum += v;
            }
            double loudest = 0.0;
            const size_t win = static_cast<size_t>(sr * 0.1);
            for (size_t s = 0; s + win <= frames; s += win / 4) {
                double e = 0.0;
                for (size_t i = s; i < s + win; ++i) e += x[2 * i] * x[2 * i];
                loudest = std::max(loudest, e / win);
            }
            const std::string id = std::string(b.id) + " at " + std::to_string(sr) + " Hz";
            if (sr == 48000)
                MESSAGE(id << ": " << seconds << " s, peak " << gainToDb(peak) << " dBFS, loudest 100 ms "
                           << 10 * std::log10(loudest + 1e-12) << " dBFS RMS");
            CHECK_MESSAGE(allFinite(x), id);
            CHECK_MESSAGE((seconds > 0.2 && seconds < 7.0), id);
            CHECK_MESSAGE(peak <= dbToGain(-0.95f), id << ": peak " << gainToDb(peak) << " dBFS");
            CHECK_MESSAGE(10 * std::log10(loudest + 1e-12) > -30.0, id);
            CHECK_MESSAGE(std::fabs(sum / x.size()) < 0.01, id); // no DC offset
            CHECK_MESSAGE(std::fabs(x[0]) < 0.01f, id);          // no click at the start
            CHECK_MESSAGE(std::fabs(x[x.size() - 2]) < 0.01f, id); // or at the end
        }
    }
    std::string err;
    CHECK(loadBuiltinSound("no_such_sound", 48000, &err).empty());
    CHECK(err == "Unknown built-in sound");
}

TEST_CASE("soundboard: the built-in pads are added once and can be brought back") {
    SoundboardSettings sb;
    CHECK(addBuiltinPads(sb) == static_cast<int>(builtinSounds().size()));
    CHECK(addBuiltinPads(sb) == 0);
    sb.pads.erase(sb.pads.begin() + 2);
    CHECK(addBuiltinPads(sb) == 1);
    std::vector<uint32_t> ids;
    for (const auto& p : sb.pads) ids.push_back(p.id);
    std::sort(ids.begin(), ids.end());
    CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
}

TEST_CASE("soundboard: sounds added to Echo later reach older boards once; removed ones stay removed") {
    // A board saved before the version was kept: the first 14 sounds, the
    // user deleted the crickets.
    SoundboardSettings sb;
    for (const auto& b : builtinSounds())
        if (b.since == 1 && std::string(b.id) != "crickets") {
            SoundPad p;
            p.id = sb.nextPadId++;
            p.name = b.name;
            p.builtin = b.id;
            sb.pads.push_back(p);
        }
    sb.initialized = true;
    json j = {{"format", "echo.session"}, {"version", 3}};
    Session s;
    s.soundboard = sb;
    j = sessionToJson(s);
    j["soundboard"].erase("builtinVersion");
    Session old;
    REQUIRE(sessionFromJson(j, old, nullptr));
    CHECK(old.soundboard.builtinVersion == 1);
    CHECK(addNewBuiltinPads(old.soundboard) == 2);
    auto has = [&](const char* id) {
        return std::any_of(old.soundboard.pads.begin(), old.soundboard.pads.end(), [&](const SoundPad& p) { return p.builtin == id; });
    };
    CHECK(has("clown_horn"));
    CHECK(has("gunshot"));
    CHECK_FALSE(has("crickets"));
    CHECK(old.soundboard.builtinVersion == kBuiltinSoundsVersion);
    CHECK(addNewBuiltinPads(old.soundboard) == 0); // once
    // A new board gets everything and is up to date.
    SoundboardSettings fresh;
    CHECK(addBuiltinPads(fresh) == static_cast<int>(builtinSounds().size()));
    CHECK(fresh.builtinVersion == kBuiltinSoundsVersion);
}

TEST_CASE("soundboard player: play, start over, toggle, hold and stop all, without clicks") {
    SoundboardSource s;
    s.prepare(48000.0, 480, 0.01);
    auto bank = std::make_shared<SoundBank>();
    bank->pads.push_back({toneClip(440.0, 1.0, 0.5f), 1.0f});
    bank->pads.push_back({toneClip(660.0, 1.0, 0.5f), 1.0f});
    s.setBank(bank);

    CHECK(pullBlocks(s, 2) == std::vector<float>(960, 0.0f)); // silent until asked
    s.play(0);
    auto y = pullBlocks(s, 10);
    CHECK(toneAmplitude(y, 4800 - 2400, 2400, 440.0, 48000.0) == doctest::Approx(0.5).epsilon(0.02));
    CHECK(s.progress(0) > 0.05f);
    CHECK(s.progress(1) < 0.0f);
    CHECK(s.playingCount() == 1);

    // Toggle stops it (10 ms fade), with no click.
    s.toggle(0);
    y = pullBlocks(s, 3);
    CHECK(maxStep(y) < 0.05f); // a 440 Hz sine at 0.5 moves at most 0.029 per sample; a hard cut jumps up to 0.5
    CHECK(std::fabs(y.back()) < 1e-6f);
    CHECK(s.progress(0) < 0.0f);

    // Two at once are mixed.
    s.play(0);
    s.play(1);
    y = pullBlocks(s, 10);
    CHECK(toneAmplitude(y, 2400, 2400, 440.0, 48000.0) == doctest::Approx(0.5).epsilon(0.03));
    CHECK(toneAmplitude(y, 2400, 2400, 660.0, 48000.0) == doctest::Approx(0.5).epsilon(0.03));
    // Pressing again starts it over: still one copy afterwards.
    s.play(1);
    pullBlocks(s, 3);
    CHECK(s.playingCount() == 2);
    s.stopAll();
    y = pullBlocks(s, 3);
    CHECK(maxStep(y) < 0.1f);
    CHECK(s.playingCount() == 0);

    // Hold: from another thread (as the hotkey thread does).
    std::thread([&] { s.play(1); }).join();
    y = pullBlocks(s, 5);
    CHECK(toneAmplitude(y, 1200, 1200, 660.0, 48000.0) == doctest::Approx(0.5).epsilon(0.03));
    std::thread([&] { s.stop(1); }).join();
    y = pullBlocks(s, 3);
    CHECK(maxStep(y) < 0.05f);
    CHECK(std::fabs(y.back()) < 1e-6f);

    // A sound plays to its end and stops by itself.
    auto shortBank = std::make_shared<SoundBank>();
    shortBank->pads.push_back({toneClip(440.0, 0.05, 0.5f), 1.0f});
    s.setBank(shortBank);
    s.play(0);
    pullBlocks(s, 10);
    CHECK(s.playingCount() == 0);
}

TEST_CASE("soundboard player: a pad removed from the board fades out at once; the others keep playing") {
    SoundboardSource s;
    s.prepare(48000.0, 480, 0.01);
    auto a = toneClip(440.0, 2.0, 0.5f), b = toneClip(660.0, 2.0, 0.5f);
    auto bank = std::make_shared<SoundBank>();
    bank->pads.push_back({a, 1.0f});
    bank->pads.push_back({b, 1.0f});
    s.setBank(bank);
    s.play(0);
    s.play(1);
    pullBlocks(s, 5);
    // The first pad is removed: the second moves to slot 0.
    auto bank2 = std::make_shared<SoundBank>();
    bank2->pads.push_back({b, 1.0f});
    s.setBank(bank2);
    bank.reset(); // the old bank may go once the engine moved on (the app keeps it in the graph snapshot)
    s.pull(480);
    const std::vector<float> y = pullBlocks(s, 5);
    CHECK(toneAmplitude(y, 0, 2400, 440.0, 48000.0) < 0.001);
    CHECK(toneAmplitude(y, 0, 2400, 660.0, 48000.0) == doctest::Approx(0.5).epsilon(0.03));
    CHECK(s.progress(0) > 0.0f);
    CHECK(s.playingCount() == 1);
}

TEST_CASE("soundboard: resampling a sound keeps its pitch and length") {
    WavData mono;
    mono.sampleRate = 44100;
    mono.channels = 1;
    mono.samples = sine(1000.0, 44100.0, 44100, 0.5f);
    const std::vector<float> out = stereoAtRate(mono, 48000); // mono to both sides, 44.1 -> 48 kHz
    CHECK(out.size() / 2 == 48000);
    std::vector<float> left(out.size() / 2);
    for (size_t i = 0; i < left.size(); ++i) left[i] = out[2 * i];
    bool same = true;
    for (size_t i = 0; i < left.size(); ++i) same = same && out[2 * i + 1] == left[i];
    CHECK(same);
    CHECK(toneAmplitude(left, 4800, 38400, 1000.0, 48000.0) == doctest::Approx(0.5).epsilon(0.01));
    CHECK(toneAmplitude(left, 4800, 38400, 918.75, 48000.0) < 0.01); // not at the old rate's pitch
}

TEST_CASE("soundboard: plays into the virtual mic and the headphones, even with the microphone off") {
    World w;
    w.mc->createDefaultSession();
    StripModel* strip = w.mc->soundboardStrip();
    REQUIRE(strip);
    CHECK(strip->source.kind == SourceKind::Soundboard);
    CHECK_FALSE(w.mc->session().soundboard.pads.empty()); // built-in pads
    w.mc->start(false);
    auto bank = std::make_shared<SoundBank>();
    bank->pads.push_back({toneClip(1000.0, 2.0, 0.5f), 1.0f});
    w.mc->setSoundBank(bank);
    w.mc->engine().setMicGate(false); // push to talk closed: only the microphone goes quiet
    w.mc->soundboard().play(0);
    w.step(20);
    CHECK(renderAmp(*w.backend.renders["cable_in"], 1000.0) == doctest::Approx(0.5).epsilon(0.05));
    CHECK(renderAmp(*w.backend.renders["phones"], 1000.0) == doctest::Approx(0.5).epsilon(0.05));
    CHECK(renderAmp(*w.backend.renders["cable_in"], 440.0) < 1e-3); // the gated microphone
    w.mc->stop();
}

TEST_CASE("soundboard: settings survive saving and loading") {
    Session s;
    addBuiltinPads(s.soundboard);
    s.soundboard.initialized = true;
    s.soundboard.pads[0].key = {0x61, 0};
    s.soundboard.pads[1].mode = PadMode::Hold;
    s.soundboard.pads[2].gainDb = -6.0f;
    SoundPad mine;
    mine.id = s.soundboard.nextPadId++;
    mine.name = "My sound";
    mine.file = "my sound-0001.wav";
    mine.key = {keycode::padKey(0, 12), 0};
    mine.mode = PadMode::Toggle;
    s.soundboard.pads.push_back(mine);
    s.soundboard.stopAllKey = {vk::XButton2, KeyModCtrl};
    Session r;
    REQUIRE(sessionFromJson(json::parse(sessionToJson(s).dump()), r, nullptr));
    REQUIRE(r.soundboard.pads.size() == s.soundboard.pads.size());
    CHECK(r.soundboard.initialized);
    CHECK(r.soundboard.stopAllKey == s.soundboard.stopAllKey);
    for (size_t i = 0; i < r.soundboard.pads.size(); ++i) {
        const SoundPad &a = s.soundboard.pads[i], &b = r.soundboard.pads[i];
        CHECK(a.id == b.id);
        CHECK(a.name == b.name);
        CHECK(a.builtin == b.builtin);
        CHECK(a.file == b.file);
        CHECK(a.key == b.key);
        CHECK(a.mode == b.mode);
        CHECK(a.gainDb == doctest::Approx(b.gainDb));
    }
    CHECK(r.soundboard.nextPadId > mine.id);
}

TEST_CASE("soundboard: sound files are read (WAV, and compressed files through Windows)") {
    win::ComScope com(COINIT_MULTITHREADED);
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "echo-test-sounds";
    std::filesystem::create_directories(dir);

    // WAV, mono 44.1 kHz.
    const std::wstring wav = (dir / "tone.wav").wstring();
    {
        WavWriter w;
        REQUIRE(w.open(wav, 44100, 1, 16));
        const auto x = sine(1000.0, 44100.0, 44100, 0.5f);
        w.write(x.data(), x.size());
        w.close();
    }
    win::DecodedAudio a;
    std::string err;
    REQUIRE(win::decodeAudioFile(wav, 60.0, a, &err));
    CHECK(a.sampleRate == 44100);
    CHECK(a.channels == 1);
    CHECK(a.samples.size() == 44100);
    CHECK(toneAmplitude(a.samples, 4410, 22050, 1000.0, 44100.0) == doctest::Approx(0.5).epsilon(0.01));
    // Longer than allowed: cut.
    REQUIRE(win::decodeAudioFile(wav, 0.5, a, &err));
    CHECK(a.truncated);
    CHECK(a.samples.size() == 22050);

    // AAC in an .m4a file, written with Windows' own encoder.
    const std::wstring m4a = (dir / "tone.m4a").wstring();
    bool written = false;
    if (SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        IMFSinkWriter* writer = nullptr;
        if (SUCCEEDED(MFCreateSinkWriterFromURL(m4a.c_str(), nullptr, nullptr, &writer))) {
            IMFMediaType *out = nullptr, *in = nullptr;
            MFCreateMediaType(&out);
            out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
            out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
            out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
            out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);
            MFCreateMediaType(&in);
            in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
            in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
            in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
            in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 48000 * 4);
            DWORD stream = 0;
            if (SUCCEEDED(writer->AddStream(out, &stream)) && SUCCEEDED(writer->SetInputMediaType(stream, in, nullptr)) &&
                SUCCEEDED(writer->BeginWriting())) {
                const auto x = sine(1000.0, 48000.0, 48000, 0.5f);
                bool ok = true;
                for (size_t pos = 0; pos < x.size() && ok; pos += 1024) {
                    const size_t n = std::min<size_t>(1024, x.size() - pos);
                    IMFMediaBuffer* buf = nullptr;
                    IMFSample* sample = nullptr;
                    MFCreateMemoryBuffer(static_cast<DWORD>(n * 4), &buf);
                    BYTE* data = nullptr;
                    buf->Lock(&data, nullptr, nullptr);
                    auto* s16 = reinterpret_cast<int16_t*>(data);
                    for (size_t i = 0; i < n; ++i) s16[2 * i] = s16[2 * i + 1] = static_cast<int16_t>(x[pos + i] * 32767.0f);
                    buf->Unlock();
                    buf->SetCurrentLength(static_cast<DWORD>(n * 4));
                    MFCreateSample(&sample);
                    sample->AddBuffer(buf);
                    sample->SetSampleTime(static_cast<LONGLONG>(pos * 10000000ull / 48000));
                    sample->SetSampleDuration(static_cast<LONGLONG>(n * 10000000ull / 48000));
                    ok = SUCCEEDED(writer->WriteSample(stream, sample));
                    sample->Release();
                    buf->Release();
                }
                written = ok && SUCCEEDED(writer->Finalize());
            }
            if (out) out->Release();
            if (in) in->Release();
            writer->Release();
        }
        MFShutdown();
    }
    if (!written) {
        MESSAGE("SKIPPED: no AAC encoder on this system");
    } else {
        REQUIRE(win::decodeAudioFile(m4a, 60.0, a, &err));
        MESSAGE("AAC decoded: " << a.sampleRate << " Hz, " << a.channels << " channels, "
                                << a.samples.size() / std::max(1, a.channels) << " frames");
        CHECK(a.sampleRate == 48000);
        CHECK(a.channels == 2);
        const size_t frames = a.samples.size() / 2;
        CHECK(frames >= 47000);
        std::vector<float> left(frames);
        for (size_t i = 0; i < frames; ++i) left[i] = a.samples[2 * i];
        CHECK(toneAmplitude(left, 9600, 24000, 1000.0, 48000.0) == doctest::Approx(0.5).epsilon(0.08));
    }
    // Not a sound file.
    const std::wstring junk = (dir / "notes.mp3").wstring();
    FILE* f = _wfopen(junk.c_str(), L"wb");
    REQUIRE(f);
    std::fputs("this is not audio", f);
    std::fclose(f);
    CHECK_FALSE(win::decodeAudioFile(junk, 60.0, a, &err));
    CHECK_FALSE(err.empty());
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
