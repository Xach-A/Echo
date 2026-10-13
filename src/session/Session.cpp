#include "session/Session.h"

#include <algorithm>

namespace echo {

const char* sourceKindName(SourceKind k) {
    switch (k) {
    case SourceKind::None: return "None";
    case SourceKind::CaptureDevice: return "Input device";
    case SourceKind::RenderLoopback: return "Output loopback";
    case SourceKind::ProcessLoopback: return "Application";
    case SourceKind::SystemExcludingEcho: return "System audio (excluding Echo)";
    case SourceKind::ToneGenerator: return "Test signal";
    case SourceKind::Soundboard: return "Soundboard";
    }
    return "?";
}

const char* stripGroupName(StripGroup g) {
    switch (g) {
    case StripGroup::Hardware: return "Hardware inputs";
    case StripGroup::Application: return "Applications";
    case StripGroup::Virtual: return "Virtual inputs";
    case StripGroup::Generator: return "Generators";
    }
    return "?";
}

std::string SourceRef::key() const {
    std::string k = std::to_string(static_cast<int>(kind)) + "|";
    switch (kind) {
    case SourceKind::CaptureDevice:
    case SourceKind::RenderLoopback: k += (deviceId.empty() ? "<default>" : deviceId) + (exclusive ? "|x" : ""); break;
    case SourceKind::ProcessLoopback:
        // By program name: the process ID is filled in once the capture opens
        // (and changes when the app restarts); keying on it reopened the
        // capture right after start-up, which caused an audible gap.
        k += processName.empty() ? "pid:" + std::to_string(processId) : processName;
        break;
    default: break;
    }
    return k;
}

StripModel* Session::strip(uint32_t id) {
    for (auto& s : strips)
        if (s.id == id) return &s;
    return nullptr;
}

BusModel* Session::bus(uint32_t id) {
    for (auto& b : buses)
        if (b.id == id) return &b;
    return nullptr;
}

BusModel* Session::busBySlot(int slot) {
    for (auto& b : buses)
        if (b.slot == slot) return &b;
    return nullptr;
}

int Session::freeBusSlot() const {
    for (int s = 0; s < kMaxBuses; ++s) {
        bool used = false;
        for (const auto& b : buses) used |= b.slot == s;
        if (!used) return s;
    }
    return -1;
}

} // namespace echo
