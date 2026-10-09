#include "session/Serialization.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "dsp/EffectRegistry.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace echo {

namespace {

const char* kindToString(SourceKind k) {
    switch (k) {
    case SourceKind::CaptureDevice: return "capture_device";
    case SourceKind::RenderLoopback: return "render_loopback";
    case SourceKind::ProcessLoopback: return "process_loopback";
    case SourceKind::SystemExcludingEcho: return "system_excluding_echo";
    case SourceKind::ToneGenerator: return "tone_generator";
    default: return "none";
    }
}

SourceKind kindFromString(const std::string& s) {
    if (s == "capture_device") return SourceKind::CaptureDevice;
    if (s == "render_loopback") return SourceKind::RenderLoopback;
    if (s == "process_loopback") return SourceKind::ProcessLoopback;
    if (s == "system_excluding_echo") return SourceKind::SystemExcludingEcho;
    if (s == "tone_generator") return SourceKind::ToneGenerator;
    return SourceKind::None;
}

const char* groupToString(StripGroup g) {
    switch (g) {
    case StripGroup::Application: return "application";
    case StripGroup::Virtual: return "virtual";
    case StripGroup::Generator: return "generator";
    default: return "hardware";
    }
}

StripGroup groupFromString(const std::string& s) {
    if (s == "application") return StripGroup::Application;
    if (s == "virtual") return StripGroup::Virtual;
    if (s == "generator") return StripGroup::Generator;
    return StripGroup::Hardware;
}

template <typename T>
T get(const json& j, const char* key, T def) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return def;
    try {
        return it->get<T>();
    } catch (...) {
        return def;
    }
}

} // namespace

json effectToJson(const Effect& e) {
    json params = json::object();
    for (int i = 0; i < e.numParams(); ++i) params[e.paramInfo(i).id] = e.param(i);
    json j = {{"type", e.typeId()}, {"bypass", e.bypass.load()}, {"params", params}};
    if (e.label != e.info().name) j["label"] = e.label;
    if (const std::string st = e.saveState(); !st.empty()) j["state"] = st;
    return j;
}

std::shared_ptr<Effect> effectFromJson(const json& j, std::vector<std::string>* warnings) {
    const std::string type = get<std::string>(j, "type", "");
    std::unique_ptr<Effect> e = EffectRegistry::instance().create(type);
    if (!e) {
        if (warnings) warnings->push_back("Unknown effect type '" + type + "' was skipped");
        return nullptr;
    }
    e->bypass = get<bool>(j, "bypass", false);
    e->label = get<std::string>(j, "label", e->info().name);
    if (const std::string st = get<std::string>(j, "state", ""); !st.empty()) e->loadState(st);
    if (auto it = j.find("params"); it != j.end() && it->is_object()) {
        for (auto p = it->begin(); p != it->end(); ++p) {
            const int idx = e->paramIndex(p.key());
            if (idx < 0) continue; // parameter removed in this version
            if (p->is_number()) e->setParam(idx, p->get<float>());
            else if (p->is_boolean()) e->setParam(idx, p->get<bool>() ? 1.0f : 0.0f);
        }
    }
    return std::shared_ptr<Effect>(std::move(e));
}

json chainToJson(const std::vector<std::shared_ptr<Effect>>& chain) {
    json a = json::array();
    for (const auto& e : chain) a.push_back(effectToJson(*e));
    return a;
}

std::vector<std::shared_ptr<Effect>> chainFromJson(const json& j, std::vector<std::string>* warnings) {
    std::vector<std::shared_ptr<Effect>> out;
    if (!j.is_array()) return out;
    for (const auto& e : j)
        if (auto fx = effectFromJson(e, warnings)) out.push_back(fx);
    return out;
}

json sessionToJson(const Session& s) {
    json j;
    j["format"] = "echo.session";
    j["version"] = s.configVersion;
    j["firstRunCompleted"] = s.firstRunCompleted;
    j["autoNoiseSuppression"] = s.autoNoiseSuppression;
    j["nextId"] = s.nextId;

    json strips = json::array();
    for (const auto& st : s.strips) {
        const StripRuntime& r = *st.rt;
        json sends = json::array();
        for (const auto& b : s.buses) {
            const SendParams& sp = r.sends[b.slot];
            sends.push_back({{"bus", b.id},
                             {"enabled", sp.enabled.load()},
                             {"gain", sp.gainDb.load()},
                             {"preFader", sp.preFader.load()}});
        }
        strips.push_back({
            {"id", st.id},
            {"name", st.name},
            {"color", st.color},
            {"group", groupToString(st.group)},
            {"source",
             {{"kind", kindToString(st.source.kind)},
              {"deviceId", st.source.deviceId},
              {"deviceName", st.source.deviceName},
              {"processName", st.source.processName},
              {"exclusive", st.source.exclusive}}},
            {"reference",
             {{"kind", kindToString(st.reference.kind)},
              {"deviceId", st.reference.deviceId},
              {"deviceName", st.reference.deviceName}}},
            {"trim", r.trimDb.load()},
            {"fader", r.faderDb.load()},
            {"pan", r.pan.load()},
            {"mute", r.mute.load()},
            {"solo", r.solo.load()},
            {"invert", r.invertPolarity.load()},
            {"fxBypass", r.fxBypass.load()},
            {"chanFirst", r.chanFirst.load()},
            {"chanCount", r.chanCount.load()},
            {"downmix", r.downmix.load()},
            {"sends", sends},
            {"chain", chainToJson(st.chain)},
        });
    }
    j["strips"] = strips;

    json buses = json::array();
    for (const auto& b : s.buses) {
        const BusRuntime& r = *b.rt;
        buses.push_back({
            {"id", b.id},
            {"slot", b.slot},
            {"name", b.name},
            {"color", b.color},
            {"kind", b.kind == BusKind::Virtual ? "virtual" : "physical"},
            {"output", {{"deviceId", b.output.deviceId}, {"deviceName", b.output.deviceName}, {"exclusive", b.output.exclusive}}},
            {"fader", r.faderDb.load()},
            {"mute", r.mute.load()},
            {"mono", r.mono.load()},
            {"limiter", r.limiterEnabled.load()},
            {"limiterCeiling", r.limiterCeilingDb.load()},
            {"fxBypass", r.fxBypass.load()},
            {"chain", chainToJson(b.chain)},
        });
    }
    j["buses"] = buses;

    json cables = json::array();
    for (const auto& c : s.cables)
        cables.push_back({{"name", c.name},
                          {"renderId", c.renderId},
                          {"renderName", c.renderName},
                          {"captureId", c.captureId},
                          {"captureName", c.captureName},
                          {"verified", c.verified}});
    j["cables"] = cables;

    j["engine"] = {{"sampleRate", s.engine.sampleRate},
                   {"blockSize", s.engine.blockSize},
                   {"resamplerQuality", static_cast<int>(s.engine.resamplerQuality)},
                   {"lowLatencyShared", s.engine.lowLatencyShared},
                   {"feedbackProtection", s.engine.feedbackProtection},
                   {"clockBusId", s.engine.clockBusId}};
    j["recorder"] = {{"folder", s.recorder.folder}, {"bitDepth", s.recorder.bitDepth}, {"sampleRate", s.recorder.sampleRate}};
    j["ui"] = {{"theme", s.ui.theme},
               {"uiScale", s.ui.uiScale},
               {"windowX", s.ui.windowX},
               {"windowY", s.ui.windowY},
               {"windowW", s.ui.windowW},
               {"windowH", s.ui.windowH},
               {"maximized", s.ui.maximized},
               {"showTooltips", s.ui.showTooltips},
               {"compactStrips", s.ui.compactStrips},
               {"accent", s.ui.accent}};
    return j;
}

bool sessionFromJson(const json& j, Session& out, std::vector<std::string>* warnings) {
    if (!j.is_object() || get<std::string>(j, "format", "") != "echo.session") return false;
    Session s;
    s.configVersion = get<int>(j, "version", 1);
    s.firstRunCompleted = get<bool>(j, "firstRunCompleted", false);
    s.autoNoiseSuppression = get<bool>(j, "autoNoiseSuppression", true);
    s.nextId = get<uint32_t>(j, "nextId", 1);

    if (auto it = j.find("buses"); it != j.end() && it->is_array()) {
        for (const auto& bj : *it) {
            if (s.buses.size() >= static_cast<size_t>(kMaxBuses)) break;
            BusModel b;
            b.id = get<uint32_t>(bj, "id", s.nextId++);
            b.slot = clampv(get<int>(bj, "slot", 0), 0, kMaxBuses - 1);
            for (const auto& other : s.buses)
                if (other.slot == b.slot) b.slot = s.freeBusSlot();
            if (b.slot < 0) continue;
            b.name = get<std::string>(bj, "name", "Bus");
            b.color = get<uint32_t>(bj, "color", b.color);
            b.kind = get<std::string>(bj, "kind", "physical") == "virtual" ? BusKind::Virtual : BusKind::Physical;
            if (auto o = bj.find("output"); o != bj.end()) {
                b.output.deviceId = get<std::string>(*o, "deviceId", "");
                b.output.deviceName = get<std::string>(*o, "deviceName", "");
                b.output.exclusive = get<bool>(*o, "exclusive", false);
            }
            b.rt->faderDb = clampv(get<float>(bj, "fader", 0.0f), -120.0f, 12.0f);
            b.rt->mute = get<bool>(bj, "mute", false);
            b.rt->mono = get<bool>(bj, "mono", false);
            b.rt->limiterEnabled = get<bool>(bj, "limiter", true);
            b.rt->limiterCeilingDb = clampv(get<float>(bj, "limiterCeiling", -0.3f), -24.0f, 0.0f);
            b.rt->fxBypass = get<bool>(bj, "fxBypass", false);
            if (auto c = bj.find("chain"); c != bj.end()) b.chain = chainFromJson(*c, warnings);
            s.nextId = std::max(s.nextId, b.id + 1);
            s.buses.push_back(std::move(b));
        }
    }

    if (auto it = j.find("strips"); it != j.end() && it->is_array()) {
        for (const auto& sj : *it) {
            if (s.strips.size() >= static_cast<size_t>(kMaxStrips)) break;
            StripModel st;
            st.id = get<uint32_t>(sj, "id", s.nextId++);
            st.name = get<std::string>(sj, "name", "Input");
            st.color = get<uint32_t>(sj, "color", st.color);
            st.group = groupFromString(get<std::string>(sj, "group", "hardware"));
            if (auto src = sj.find("source"); src != sj.end()) {
                st.source.kind = kindFromString(get<std::string>(*src, "kind", "none"));
                st.source.deviceId = get<std::string>(*src, "deviceId", "");
                st.source.deviceName = get<std::string>(*src, "deviceName", "");
                st.source.processName = get<std::string>(*src, "processName", "");
                st.source.exclusive = get<bool>(*src, "exclusive", false);
            }
            if (auto ref = sj.find("reference"); ref != sj.end() && ref->is_object()) {
                st.reference.kind = kindFromString(get<std::string>(*ref, "kind", "none"));
                st.reference.deviceId = get<std::string>(*ref, "deviceId", "");
                st.reference.deviceName = get<std::string>(*ref, "deviceName", "");
            }
            StripRuntime& r = *st.rt;
            r.trimDb = clampv(get<float>(sj, "trim", 0.0f), -60.0f, 36.0f);
            r.faderDb = clampv(get<float>(sj, "fader", 0.0f), -120.0f, 12.0f);
            r.pan = clampv(get<float>(sj, "pan", 0.0f), -1.0f, 1.0f);
            r.mute = get<bool>(sj, "mute", false);
            r.solo = get<bool>(sj, "solo", false);
            r.invertPolarity = get<bool>(sj, "invert", false);
            r.fxBypass = get<bool>(sj, "fxBypass", false);
            r.chanFirst = clampv(get<int>(sj, "chanFirst", 0), 0, kMaxSourceChannels - 1);
            r.chanCount = clampv(get<int>(sj, "chanCount", 2), 1, 2);
            r.downmix = get<bool>(sj, "downmix", false);
            if (auto sends = sj.find("sends"); sends != sj.end() && sends->is_array()) {
                for (const auto& sd : *sends) {
                    const uint32_t busId = get<uint32_t>(sd, "bus", 0);
                    BusModel* b = s.bus(busId);
                    if (!b) continue;
                    SendParams& sp = r.sends[b->slot];
                    sp.enabled = get<bool>(sd, "enabled", false);
                    sp.gainDb = clampv(get<float>(sd, "gain", 0.0f), -120.0f, 12.0f);
                    sp.preFader = get<bool>(sd, "preFader", false);
                }
            }
            if (auto c = sj.find("chain"); c != sj.end()) st.chain = chainFromJson(*c, warnings);
            s.nextId = std::max(s.nextId, st.id + 1);
            s.strips.push_back(std::move(st));
        }
    }

    if (auto it = j.find("cables"); it != j.end() && it->is_array()) {
        for (const auto& cj : *it) {
            VirtualCableModel c;
            c.name = get<std::string>(cj, "name", "Virtual Cable");
            c.renderId = get<std::string>(cj, "renderId", "");
            c.renderName = get<std::string>(cj, "renderName", "");
            c.captureId = get<std::string>(cj, "captureId", "");
            c.captureName = get<std::string>(cj, "captureName", "");
            c.verified = get<bool>(cj, "verified", false);
            s.cables.push_back(std::move(c));
        }
    }

    if (auto e = j.find("engine"); e != j.end()) {
        s.engine.sampleRate = clampv(get<int>(*e, "sampleRate", 48000), 8000, 192000);
        s.engine.blockSize = clampv(get<int>(*e, "blockSize", 480), 32, kMaxBlockSize);
        s.engine.resamplerQuality =
            static_cast<ResamplerQuality>(clampv(get<int>(*e, "resamplerQuality", 1), 0, 2));
        s.engine.lowLatencyShared = get<bool>(*e, "lowLatencyShared", false);
        s.engine.feedbackProtection = get<bool>(*e, "feedbackProtection", true);
        s.engine.clockBusId = get<uint32_t>(*e, "clockBusId", 0);
    }
    if (auto r = j.find("recorder"); r != j.end()) {
        s.recorder.folder = get<std::string>(*r, "folder", "");
        s.recorder.bitDepth = get<int>(*r, "bitDepth", 24);
        if (s.recorder.bitDepth != 16 && s.recorder.bitDepth != 24 && s.recorder.bitDepth != 32) s.recorder.bitDepth = 24;
        s.recorder.sampleRate = get<int>(*r, "sampleRate", 0);
    }
    if (auto u = j.find("ui"); u != j.end()) {
        s.ui.theme = get<int>(*u, "theme", 0);
        s.ui.uiScale = clampv(get<float>(*u, "uiScale", 1.0f), 0.6f, 2.5f);
        s.ui.windowX = get<int>(*u, "windowX", -1);
        s.ui.windowY = get<int>(*u, "windowY", -1);
        s.ui.windowW = get<int>(*u, "windowW", 1480);
        s.ui.windowH = get<int>(*u, "windowH", 900);
        s.ui.maximized = get<bool>(*u, "maximized", false);
        s.ui.showTooltips = get<bool>(*u, "showTooltips", true);
        s.ui.compactStrips = get<bool>(*u, "compactStrips", false);
        s.ui.accent = get<std::string>(*u, "accent", "blue");
    }
    out = std::move(s);
    return true;
}

bool saveJsonFile(const std::wstring& path, const json& j, std::string* error) {
    const std::wstring tmp = path + L".tmp";
    const std::wstring bak = path + L".bak";
    {
        std::ofstream f(std::filesystem::path(tmp), std::ios::binary | std::ios::trunc);
        if (!f) {
            if (error) *error = "cannot write temporary file";
            return false;
        }
        f << j.dump(2);
        f.flush();
        if (!f) {
            if (error) *error = "write failed (disk full?)";
            return false;
        }
    }
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) CopyFileW(path.c_str(), bak.c_str(), FALSE);
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (error) *error = "cannot replace settings file";
        return false;
    }
    return true;
}

bool loadJsonFile(const std::wstring& path, json& out, std::string* error, bool tryBackup) {
    auto readOne = [&](const std::wstring& p, std::string* err) {
        std::ifstream f(std::filesystem::path(p), std::ios::binary);
        if (!f) {
            if (err) *err = "file not found";
            return false;
        }
        std::stringstream ss;
        ss << f.rdbuf();
        try {
            out = json::parse(ss.str());
            return true;
        } catch (const std::exception& ex) {
            if (err) *err = std::string("invalid JSON: ") + ex.what();
            return false;
        }
    };
    if (readOne(path, error)) return true;
    if (tryBackup && readOne(path + L".bak", nullptr)) {
        if (error) *error += " (loaded backup instead)";
        return true;
    }
    return false;
}

} // namespace echo
