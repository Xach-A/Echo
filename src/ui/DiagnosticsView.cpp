// Diagnostics: engine health, per-device buffer statistics, latency
// estimates, logs and a shareable text report.
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "Version.h"
#include "core/Log.h"
#include "core/Text.h"
#include "imgui.h"
#include "session/Paths.h"
#include "ui/App.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

namespace echo::ui {

std::string EchoApp::diagnosticReport() {
    std::ostringstream o;
    Engine& e = mc_->engine();
    std::time_t t = std::time(nullptr);
    char stamp[64];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    o << ECHO_APP_NAME << " " << ECHO_VERSION_STRING << " diagnostic report, " << stamp << "\n";
    OSVERSIONINFOEXW vi{sizeof(vi)};
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOEXW*);
    if (auto fn = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))))
        fn(&vi);
    o << "Windows " << vi.dwMajorVersion << "." << vi.dwMinorVersion << " build " << vi.dwBuildNumber << "\n";
    o << "Engine: " << (e.running() ? "running" : "stopped") << ", " << e.sampleRate() << " Hz, block " << e.blockSize()
      << ", " << mc_->clockDescription() << "\n";
    o << "DSP load " << e.stats().cpuLoad.load() * 100.0f << "%, blocks " << e.stats().blocks.load() << ", late "
      << e.stats().lateBlocks.load() << ", device stalls " << e.stats().deviceStalls.load() << ", exceptions "
      << e.stats().exceptions.load() << "\n\nInputs:\n";
    for (const auto& s : mc_->session().strips) {
        o << "  [" << s.name << "] source=" << sourceKindName(s.source.kind) << " '" << s.source.deviceName
          << s.source.processName << "' status=" << s.status << " fx=" << s.chain.size();
        if (const SourceRuntime* r = mc_->sourceFor(s.id))
            o << " fill=" << r->fillMs.load() << "ms drift=" << r->driftPpm.load() << "ppm under=" << r->underruns.load()
              << " over=" << r->overruns.load() << " margin=+" << r->marginMs.load() << "ms latency="
              << r->latencySeconds() * 1000.0 << "ms";
        o << "\n";
    }
    o << "Buses:\n";
    for (const auto& b : mc_->session().buses) {
        o << "  [" << busLabel(b) << " " << b.name << "] device='" << b.output.deviceName << "' status=" << b.status
          << (b.isClock ? " (clock)" : "");
        if (const SinkRuntime* k = mc_->sinkFor(b.id))
            o << " fill=" << k->fillMs.load() << "ms drift=" << k->driftPpm.load() << "ppm under=" << k->underruns.load()
              << " over=" << k->overruns.load() << " margin=+" << k->marginMs.load() << "ms latency="
              << k->latencySeconds() * 1000.0 << "ms";
        o << "\n";
    }
    o << "Feedback warnings: " << mc_->feedbackWarnings().size() << "\n";
    for (const auto& w : mc_->feedbackWarnings()) o << "  " << w.text << "\n";
    o << "\nDevices:\n";
    for (const auto& d : backend_->deviceManager().devices())
        o << "  " << (d.flow == win::Flow::Render ? "OUT " : "IN  ") << (d.active() ? "active   " : "inactive ") << d.name
          << " [" << win::formatToString(d.mixFormat) << "]" << (d.isDefault ? " default" : "")
          << (d.virtualKind == win::VirtualKind::Cable ? " cable" : d.virtualKind == win::VirtualKind::AppBound ? " app-virtual" : "")
          << "\n";
    o << "\nRecent log:\n";
    for (const auto& l : log::recent(80)) o << "  " << l.text << "\n";
    return o.str();
}

void EchoApp::drawDiagnostics() {
    if (!ImGui::Begin("Diagnostics")) {
        ImGui::End();
        return;
    }
    const Palette& p = pal();
    Engine& e = mc_->engine();
    EngineStats& st = e.stats();
    cpuHistory_.push_back(st.cpuLoad.load() * 100.0f);
    if (cpuHistory_.size() > 300) cpuHistory_.erase(cpuHistory_.begin());

    const float peak = st.cpuPeak.exchange(0.0f) * 100.0f;
    static float shownPeak = 0.0f;
    shownPeak = std::max(peak, shownPeak * 0.995f);
    ImGui::Text("Engine: %s  |  %d Hz  |  block %d (%.2f ms)  |  %s", e.running() ? "running" : "stopped", e.sampleRate(),
                e.blockSize(), e.blockSeconds() * 1000.0, mc_->clockDescription().c_str());
    ImGui::Text("DSP load %.1f%% (peak %.1f%%), last block %.0f us  |  blocks %llu  |  late %llu  |  device stalls %llu  |  exceptions %llu",
                st.cpuLoad.load() * 100.0f, shownPeak, st.blockMicros.load(), static_cast<unsigned long long>(st.blocks.load()),
                static_cast<unsigned long long>(st.lateBlocks.load()), static_cast<unsigned long long>(st.deviceStalls.load()),
                static_cast<unsigned long long>(st.exceptions.load()));
    ImGui::PlotLines("##cpu", cpuHistory_.data(), static_cast<int>(cpuHistory_.size()), 0, "DSP load % (last ~5 s)", 0.0f,
                     std::max(20.0f, shownPeak * 1.2f), ImVec2(-1, 60 * dpi_));

    if (ImGui::BeginTabBar("##diagtabs")) {
        if (ImGui::BeginTabItem("Streams")) {
            if (ImGui::BeginTable("##streams", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable)) {
                for (const char* h : {"Stream", "Status", "Format", "Buffer", "Drift", "Underruns", "Overruns", "Est. latency"})
                    ImGui::TableSetupColumn(h);
                ImGui::TableHeadersRow();
                for (const auto& s : mc_->session().strips) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%s  %s", ICON_MIC, s.name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextColored(toVec4(s.online ? p.good : p.bad), "%s", s.status.c_str());
                    const SourceRuntime* r = mc_->sourceFor(s.id);
                    const CaptureEndpoint* ep = mc_->captureEndpointFor(s.id);
                    ImGui::TableNextColumn();
                    if (ep) ImGui::Text("%d Hz %d ch", ep->sampleRate(), ep->channels());
                    ImGui::TableNextColumn();
                    if (r) {
                        ImGui::Text("%.1f ms", r->fillMs.load());
                        if (r->marginMs.load() > 0.0f) {
                            ImGui::SameLine();
                            ImGui::TextDisabled("(+%.0f)", r->marginMs.load());
                            Tip("Extra buffering added after a dropout; removed again after 30 minutes without problems.");
                        }
                    }
                    ImGui::TableNextColumn();
                    if (r) ImGui::Text("%+.0f ppm", r->driftPpm.load());
                    ImGui::TableNextColumn();
                    if (r) ImGui::Text("%llu", static_cast<unsigned long long>(r->underruns.load()));
                    ImGui::TableNextColumn();
                    if (r) ImGui::Text("%llu / %llu", static_cast<unsigned long long>(r->overruns.load()),
                                       static_cast<unsigned long long>(ep ? ep->stats.overruns.load() : 0));
                    ImGui::TableNextColumn();
                    if (r) ImGui::Text("%.1f ms", r->latencySeconds() * 1000.0);
                }
                for (const auto& b : mc_->session().buses) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%s  %s %s%s", ICON_SPEAKERS, busLabel(b).c_str(), b.name.c_str(), b.isClock ? " (clock)" : "");
                    ImGui::TableNextColumn();
                    ImGui::TextColored(toVec4(b.online ? p.good : p.bad), "%s", b.status.c_str());
                    const SinkRuntime* k = mc_->sinkFor(b.id);
                    const RenderEndpoint* ep = mc_->renderEndpointFor(b.id);
                    ImGui::TableNextColumn();
                    if (ep) ImGui::Text("%d Hz %d ch", ep->sampleRate(), ep->channels());
                    ImGui::TableNextColumn();
                    if (k) {
                        ImGui::Text("%.1f ms", k->fillMs.load());
                        if (k->marginMs.load() > 0.0f) {
                            ImGui::SameLine();
                            ImGui::TextDisabled("(+%.0f)", k->marginMs.load());
                            Tip("Extra buffering added after a dropout; removed again after 30 minutes without problems.");
                        }
                    }
                    ImGui::TableNextColumn();
                    if (k) ImGui::Text("%+.0f ppm", k->driftPpm.load());
                    ImGui::TableNextColumn();
                    if (k) ImGui::Text("%llu / %llu", static_cast<unsigned long long>(k->underruns.load()),
                                       static_cast<unsigned long long>(ep ? ep->stats.underruns.load() : 0));
                    ImGui::TableNextColumn();
                    if (k) ImGui::Text("%llu", static_cast<unsigned long long>(k->overruns.load()));
                    ImGui::TableNextColumn();
                    if (k) ImGui::Text("%.1f ms", k->latencySeconds() * 1000.0);
                }
                ImGui::EndTable();
            }
            ImGui::TextWrapped("Underruns: a device ran out of audio (crackle). Overruns: audio had to be dropped. "
                               "Drift: clock-rate correction applied by Echo's resampler to keep devices in sync. "
                               "Latency values are estimates from buffer sizes, not measurements.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Latency")) {
            Session& s = mc_->session();
            if (!s.strips.empty() && !s.buses.empty()) {
                diagStrip_ = std::clamp(diagStrip_, 0, static_cast<int>(s.strips.size()) - 1);
                diagBus_ = std::clamp(diagBus_, 0, static_cast<int>(s.buses.size()) - 1);
                ImGui::SetNextItemWidth(220 * dpi_);
                if (ImGui::BeginCombo("From input", s.strips[diagStrip_].name.c_str())) {
                    for (int i = 0; i < static_cast<int>(s.strips.size()); ++i)
                        if (ImGui::Selectable(s.strips[i].name.c_str(), i == diagStrip_)) diagStrip_ = i;
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(220 * dpi_);
                if (ImGui::BeginCombo("To bus", s.buses[diagBus_].name.c_str())) {
                    for (int i = 0; i < static_cast<int>(s.buses.size()); ++i)
                        if (ImGui::Selectable(s.buses[i].name.c_str(), i == diagBus_)) diagBus_ = i;
                    ImGui::EndCombo();
                }
                ImGui::Text("Estimated latency: %.1f ms", mc_->estimatedLatencyMs(s.strips[diagStrip_].id, s.buses[diagBus_].id));
                ImGui::TextWrapped("Includes device buffers, Echo's input/output cushions, one processing block and "
                                   "effect latency. Windows' internal audio engine and the device hardware add a few "
                                   "more milliseconds. For a measured round trip, run 'echo_hwtest playback' (output "
                                   "via loopback) or the cable loopback test in Devices.");
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Log")) {
            if (ImGui::Button(ICON_FOLDER " Open log folder"))
                ShellExecuteW(nullptr, L"open", (paths::localDataDir() + L"\\logs").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            ImGui::SameLine();
            if (ImGui::Button(ICON_SAVE " Save diagnostic report")) {
                const std::wstring path = paths::localDataDir() + L"\\echo-diagnostics.txt";
                std::ofstream f{std::filesystem::path(path)};
                f << diagnosticReport();
                if (f) {
                    toast("Report saved to " + wideToUtf8(path), 1);
                    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                } else toast("Could not write the report", 3);
            }
            Tip("Writes a plain-text report (settings summary, devices, statistics, recent log). Nothing is sent anywhere.");
            ImGui::SameLine();
            if (ImGui::Button(ICON_COPY " Copy report")) ImGui::SetClipboardText(diagnosticReport().c_str());
            ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_Borders);
            ImGui::PushFont(fonts().mono, 0.0f);
            for (const auto& l : log::recent(400)) {
                const ImU32 c = l.level == log::Level::Error ? p.bad : l.level == log::Level::Warn ? p.warn : p.textDim;
                ImGui::TextColored(toVec4(c), "%8.1f  %s", l.time, l.text.c_str());
            }
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 5) ImGui::SetScrollHereY(1.0f);
            ImGui::PopFont();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

} // namespace echo::ui
