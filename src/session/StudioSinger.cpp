#include "session/StudioSinger.h"

#include <algorithm>
#include <cmath>

namespace echo::singer {

namespace {

float get(const Effect& e, const char* id) {
    const int i = e.paramIndex(id);
    return i >= 0 ? e.param(i) : 0.0f;
}

void set(Effect& e, const char* id, float v) {
    const int i = e.paramIndex(id);
    if (i >= 0) e.setParam(i, v);
}

float clamp100(float v, float lo = 0.0f) { return std::clamp(v, lo, 100.0f); }

} // namespace

float tuneFeel(const Effect& autotune) {
    return clamp100(100.0f * (1.0f - std::sqrt(std::max(0.0f, get(autotune, "speed")) / 200.0f)));
}

void setTuneFeel(Effect& autotune, float feel) {
    const float f = 1.0f - clamp100(feel) / 100.0f;
    set(autotune, "style", 2.0f); // Custom: the retune speed counts
    set(autotune, "speed", std::round(200.0f * f * f));
}

const char* tuneFeelName(float feel) {
    return feel < 20.0f ? "Natural" : feel < 45.0f ? "Smooth" : feel < 75.0f ? "Polished" : feel < 95.0f ? "Tight" : "Robotic";
}

void setKey(Effect& autotune, Effect* harmonizer, int key, int scale) {
    set(autotune, "key", static_cast<float>(key));
    set(autotune, "scale", static_cast<float>(scale));
    if (!harmonizer) return;
    const Harmony h = harmony(*harmonizer);
    if (h == Harmony::Off) {
        set(*harmonizer, "key", static_cast<float>(key));
        set(*harmonizer, "scale", static_cast<float>(scale));
        set(*harmonizer, "mode", scale == 0 ? 0.0f : 1.0f);
    } else {
        setHarmony(*harmonizer, h, key, scale);
    }
}

float tone(const Effect& eq) { return std::clamp((get(eq, "b6_gain") - 3.0f) / 0.05f, -100.0f, 100.0f); }

void setTone(Effect& eq, float t) {
    t = std::clamp(t, -100.0f, 100.0f);
    set(eq, "b1_gain", 1.0f - 0.02f * t);
    set(eq, "b4_gain", 2.5f + 0.025f * t);
    set(eq, "b6_gain", 3.0f + 0.05f * t);
}

float compression(const Effect& compressor) { return clamp100((-12.0f - get(compressor, "threshold")) / 0.24f); }

void setCompression(Effect& compressor, float amount) {
    const float a = clamp100(amount);
    set(compressor, "threshold", -12.0f - 0.24f * a);
    set(compressor, "ratio", 2.0f + 0.03f * a);
}

float saturation(const Effect& distortion) { return clamp100(get(distortion, "drive") / 0.24f); }

void setSaturation(Effect& distortion, float amount) {
    const float drive = 0.24f * clamp100(amount);
    set(distortion, "drive", drive);
    set(distortion, "output", -0.84f * drive); // measured: about as loud at any drive
}

const char* harmonyName(Harmony h) {
    switch (h) {
    case Harmony::Off: return "Off";
    case Harmony::ThirdAbove: return "A third above";
    case Harmony::ThirdBelow: return "A third below";
    case Harmony::ThirdAndFifth: return "A third and a fifth above";
    case Harmony::OctaveBelow: return "An octave below";
    default: return "";
    }
}

Harmony harmony(const Effect& harmonizer) {
    if (harmonizer.bypass.load()) return Harmony::Off;
    if (get(harmonizer, "level2") > -50.0f) return Harmony::ThirdAndFifth;
    const bool inKey = get(harmonizer, "mode") >= 0.5f;
    const float v = inKey ? get(harmonizer, "steps1") : get(harmonizer, "interval1");
    if (v <= (inKey ? -7.0f : -12.0f)) return Harmony::OctaveBelow;
    return v < 0.0f ? Harmony::ThirdBelow : Harmony::ThirdAbove;
}

void setHarmony(Effect& harmonizer, Harmony h, int key, int scale) {
    set(harmonizer, "key", static_cast<float>(key));
    set(harmonizer, "scale", static_cast<float>(scale));
    set(harmonizer, "mode", scale == 0 ? 0.0f : 1.0f); // chromatic: no key to follow
    if (h == Harmony::Off) {
        harmonizer.bypass = true;
        return;
    }
    struct Voices {
        float steps1, interval1, steps2, interval2, level2;
    };
    Voices v{2, 4, 4, 7, -60};
    if (h == Harmony::ThirdBelow) v = {-2, -4, 4, 7, -60};
    else if (h == Harmony::ThirdAndFifth) v = {2, 4, 4, 7, -12};
    else if (h == Harmony::OctaveBelow) v = {-7, -12, 4, 7, -60};
    set(harmonizer, "steps1", v.steps1);
    set(harmonizer, "interval1", v.interval1);
    set(harmonizer, "steps2", v.steps2);
    set(harmonizer, "interval2", v.interval2);
    set(harmonizer, "level1", -10.0f);
    set(harmonizer, "level2", v.level2);
    harmonizer.bypass = false;
}

} // namespace echo::singer
