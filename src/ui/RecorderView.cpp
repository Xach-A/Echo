// Recorder: record any strips (pre/post fader) and buses to WAV.
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include "core/Text.h"
#include "imgui.h"
#include "ui/App.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

namespace echo::ui {

namespace {
uint64_t key(int kind, uint32_t id) { return (static_cast<uint64_t>(kind) << 32) | id; }

bool browseFolder(HWND owner, std::string& folder) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Choose recording folder");
    bool ok = false;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                folder = wideToUtf8(path);
                CoTaskMemFree(path);
                ok = true;
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}
} // namespace

void EchoApp::drawRecorder() {
    if (!ImGui::Begin("Recorder")) {
        ImGui::End();
        return;
    }
    const Palette& p = pal();
    Session& s = mc_->session();
    Recorder& rec = mc_->recorder();
    const bool active = rec.active();

    ImGui::BeginDisabled(active);
    ImGui::TextDisabled("Record these signals (one WAV file each):");
    if (ImGui::BeginTable("##rectargets", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Signal", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Post-fader", ImGuiTableColumnFlags_WidthFixed, 90 * dpi_);
        ImGui::TableSetupColumn("Pre-fader", ImGuiTableColumnFlags_WidthFixed, 90 * dpi_);
        ImGui::TableHeadersRow();
        for (const auto& st : s.strips) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s  Input: %s", ICON_MIC, st.name.c_str());
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(st.id));
            bool& post = recordSelection_[key(RecordTap::StripPost, st.id)];
            ImGui::Checkbox("##post", &post);
            ImGui::TableNextColumn();
            bool& pre = recordSelection_[key(RecordTap::StripPre, st.id)];
            ImGui::Checkbox("##pre", &pre);
            Tip("Pre-fader: after effects, before the strip's fader, pan and mute.");
            ImGui::PopID();
        }
        for (const auto& b : s.buses) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s  Bus %s: %s", ICON_SPEAKERS, busLabel(b).c_str(), b.name.c_str());
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(b.id) + 100000);
            bool& on = recordSelection_[key(RecordTap::Bus, b.id)];
            ImGui::Checkbox("##bus", &on);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("-");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::SetNextItemWidth(160 * dpi_);
    int depthIdx = s.recorder.bitDepth == 16 ? 0 : s.recorder.bitDepth == 24 ? 1 : 2;
    const char* depths[] = {"16-bit PCM", "24-bit PCM", "32-bit float"};
    if (ImGui::Combo("Format", &depthIdx, depths, 3)) s.recorder.bitDepth = depthIdx == 0 ? 16 : depthIdx == 1 ? 24 : 32;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160 * dpi_);
    const int rates[] = {0, 44100, 48000, 96000};
    const char* rateNames[] = {"Engine rate", "44.1 kHz", "48 kHz", "96 kHz"};
    int ri = 0;
    for (int i = 0; i < 4; ++i)
        if (rates[i] == s.recorder.sampleRate) ri = i;
    if (ImGui::Combo("Sample rate", &ri, rateNames, 4)) s.recorder.sampleRate = rates[ri];
    ImGui::TextDisabled("Folder: %s", s.recorder.folder.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_FOLDER " Change...")) {
        std::string f = s.recorder.folder;
        if (browseFolder(hwnd_, f)) s.recorder.folder = f;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_OPEN " Open folder")) {
        CreateDirectoryW(utf8ToWide(s.recorder.folder).c_str(), nullptr);
        ShellExecuteW(nullptr, L"open", utf8ToWide(s.recorder.folder).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::Separator();

    if (!active) {
        if (ImGui::Button(ICON_RECORD "  Start recording", ImVec2(200 * dpi_, 36 * dpi_))) {
            std::vector<RecordingTarget> targets;
            for (const auto& st : s.strips) {
                if (recordSelection_[key(RecordTap::StripPost, st.id)]) targets.push_back({RecordTap::StripPost, st.id, st.name});
                if (recordSelection_[key(RecordTap::StripPre, st.id)]) targets.push_back({RecordTap::StripPre, st.id, st.name + " (pre)"});
            }
            for (const auto& b : s.buses)
                if (recordSelection_[key(RecordTap::Bus, b.id)]) targets.push_back({RecordTap::Bus, b.id, busLabel(b) + " " + b.name});
            std::string err;
            if (!mc_->startRecording(targets, &err)) toast("Cannot start recording: " + err, 3);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Shortcut: Ctrl+R stops a running recording.");
        ImGui::TextWrapped("Recording runs on a background thread; the audio engine never waits for the disk. Echo "
                           "never records anything unless you press Start, and recordings stay on this computer.");
    } else {
        const RecordingStatus st = rec.status();
        if (ImGui::Button(ICON_STOP "  Stop recording", ImVec2(200 * dpi_, 36 * dpi_))) {
            mc_->stopRecording();
            toast("Recording saved", 1);
        }
        ImGui::SameLine();
        ImGui::PushFont(fonts().mono, fonts().baseSize * 1.6f);
        ImGui::TextColored(toVec4(p.bad), "%s", formatDuration(st.seconds).c_str());
        ImGui::PopFont();
        ImGui::Text("%s written", formatBytes(st.bytes).c_str());
        if (st.droppedFrames) ImGui::TextColored(toVec4(p.warn), "%llu frames dropped (disk too slow)", static_cast<unsigned long long>(st.droppedFrames));
        if (!st.error.empty()) ImGui::TextColored(toVec4(p.bad), "%s", st.error.c_str());
        for (const auto& f : st.files) ImGui::TextDisabled("%s", wideToUtf8(f).c_str());
    }
    ImGui::End();
}

} // namespace echo::ui
