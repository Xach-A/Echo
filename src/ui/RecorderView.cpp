// Recorder: record outputs and inputs (with or without their effects) to WAV
// files, and the recent recordings in the folder.
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <ctime>

#include "core/Text.h"
#include "imgui.h"
#include "ui/App.h"
#include "ui/Icons.h"
#include "ui/Pop.h"
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

std::string clock(double seconds) {
    const int s = static_cast<int>(seconds);
    char buf[24];
    if (s >= 3600) std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    else std::snprintf(buf, sizeof(buf), "%02d:%02d", s / 60, s % 60);
    return buf;
}
} // namespace

void EchoApp::drawRecorder() {
    const Palette& p = pal();
    const float sc = uiScale();
    Session& s = mc_->session();
    Recorder& rec = mc_->recorder();
    const bool active = rec.active();
    pop::PageTitle("Recorder", "Records only while you press Record. Each source becomes its own WAV file on this PC.");
    ImGui::Dummy(ImVec2(0, 2.0f * sc));

    auto collectTargets = [&]() {
        std::vector<RecordingTarget> targets;
        for (const auto& b : s.buses)
            if (recordSelection_[key(RecordTap::Bus, b.id)]) targets.push_back({RecordTap::Bus, b.id, busLabel(b) + " " + b.name});
        for (const auto& st : s.strips) {
            if (recordSelection_[key(RecordTap::StripPost, st.id)]) targets.push_back({RecordTap::StripPost, st.id, st.name});
            if (recordSelection_[key(RecordTap::StripPre, st.id)]) targets.push_back({RecordTap::StripPre, st.id, st.name + " (pre)"});
            if (recordSelection_[key(RecordTap::StripRaw, st.id)]) targets.push_back({RecordTap::StripRaw, st.id, st.name + " (no effects)"});
        }
        return targets;
    };
    // First visit: the virtual mic (what others hear) is ticked.
    static bool defaulted = false;
    if (!defaulted) {
        defaulted = true;
        if (BusModel* b1 = firstBus(BusKind::Virtual)) recordSelection_[key(RecordTap::Bus, b1->id)] = true;
    }
    const size_t count = collectTargets().size();

    const float avail = ImGui::GetContentRegionAvail().x;
    const float gap = 20.0f * sc;
    const bool wide = avail > 860.0f * sc;
    const float leftW = wide ? (avail - gap) * 0.45f : avail;

    // ------------------------------------------------------------ record card
    pop::BeginCard("##rec", leftW, 0.0f, false, 24.0f, 24.0f);
    {
        const float inner = ImGui::GetContentRegionAvail().x;
        const float d = 112.0f * sc;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (inner - d) * 0.5f);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::BeginDisabled(!active && count == 0);
        const bool clicked = ImGui::InvisibleButton("##recbtn", ImVec2(d, d));
        ImGui::EndDisabled();
        Tip(active ? "Stop and save (Ctrl+R)" : "Start recording");
        const ImVec2 c(pos.x + d * 0.5f, pos.y + d * 0.5f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddCircleFilled(ImVec2(c.x + 3 * sc, c.y + 3 * sc), d * 0.5f, p.shadow, 48);
        dl->AddCircleFilled(c, d * 0.5f, active ? p.surface : (count ? p.accent : p.segBg), 48);
        dl->AddCircle(c, d * 0.5f, p.outline, 48, 1.5f * sc);
        if (active) dl->AddRectFilled(ImVec2(c.x - 17 * sc, c.y - 17 * sc), ImVec2(c.x + 17 * sc, c.y + 17 * sc), p.primary, 8.0f * sc);
        else dl->AddCircleFilled(c, 20.0f * sc, IM_COL32(255, 255, 255, 255), 32);
        if (clicked) {
            if (active) {
                mc_->stopRecording();
                toast("Recording saved", 1);
            } else {
                std::string err;
                if (!mc_->startRecording(collectTargets(), &err)) toast("Can't start recording: " + err, 3);
            }
        }
        const RecordingStatus st = rec.status();
        const std::string t = clock(active ? st.seconds : 0.0);
        ImGui::PushFont(fonts().semibold, 40.0f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (inner - ImGui::CalcTextSize(t.c_str()).x) * 0.5f);
        ImGui::TextUnformatted(t.c_str());
        ImGui::PopFont();
        char hint[96];
        if (active) std::snprintf(hint, sizeof(hint), "Recording · click to stop and save");
        else if (count) std::snprintf(hint, sizeof(hint), "Click to record %zu file%s", count, count == 1 ? "" : "s");
        else std::snprintf(hint, sizeof(hint), "Pick something to record");
        ImGui::PushFont(fonts().regular, 14.0f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (inner - ImGui::CalcTextSize(hint).x) * 0.5f);
        ImGui::TextColored(toVec4(p.text2), "%s", hint);
        ImGui::PopFont();
        if (active) {
            char info[96];
            std::snprintf(info, sizeof(info), "%s written · %llu frames dropped", formatBytes(st.bytes).c_str(),
                          static_cast<unsigned long long>(st.droppedFrames));
            ImGui::PushFont(fonts().regular, 12.0f);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (inner - ImGui::CalcTextSize(info).x) * 0.5f);
            ImGui::TextColored(toVec4(st.droppedFrames ? p.warnInk : p.textDim), "%s", info);
            ImGui::PopFont();
            if (!st.error.empty()) pop::Banner(ICON_WARNING, st.error.c_str(), p.badTint, p.bad);
        } else {
            ImGui::PushFont(fonts().regular, 12.0f);
            const char* k = "Ctrl+R stops a running recording";
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (inner - ImGui::CalcTextSize(k).x) * 0.5f);
            ImGui::TextColored(toVec4(p.textDim), "%s", k);
            ImGui::PopFont();
        }
    }
    pop::EndCard();

    // ------------------------------------------------------- what to record
    if (wide) ImGui::SameLine(0, gap);
    else ImGui::Dummy(ImVec2(0, 4.0f * sc));
    pop::BeginCard("##what", wide ? avail - leftW - gap : avail);
    pop::CardTitle("What to record");
    ImGui::BeginDisabled(active);
    for (const auto& b : s.buses) {
        bool& on = recordSelection_[key(RecordTap::Bus, b.id)];
        ImGui::PushID(static_cast<int>(b.id) + 100000);
        pop::CheckRow("##bus", (b.name + " (" + busLabel(b) + ")").c_str(),
                      b.kind == BusKind::Virtual ? "Exactly what others hear" : "Everything you hear", &on);
        ImGui::PopID();
    }
    for (const auto& st : s.strips) {
        ImGui::PushID(static_cast<int>(st.id));
        pop::CheckRow("##post", (st.name + " · with effects").c_str(), "After its effects, volume and mute",
                      &recordSelection_[key(RecordTap::StripPost, st.id)]);
        pop::CheckRow("##raw", (st.name + " · no effects").c_str(), "Before its effects, e.g. your natural voice",
                      &recordSelection_[key(RecordTap::StripRaw, st.id)]);
        ImGui::PopID();
    }
    if (ImGui::TreeNode("Also: inputs before their volume")) {
        for (const auto& st : s.strips) {
            ImGui::PushID(static_cast<int>(st.id));
            ImGui::Checkbox((st.name + " · after effects, before volume and pan").c_str(), &recordSelection_[key(RecordTap::StripPre, st.id)]);
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
    ImGui::Dummy(ImVec2(0, 2.0f * sc));
    const float half = (ImGui::GetContentRegionAvail().x - 10.0f * sc) * 0.5f;
    const float rowY = ImGui::GetCursorPosY();
    ImGui::BeginGroup();
    pop::Label("Format");
    int depthIdx = s.recorder.bitDepth == 16 ? 0 : s.recorder.bitDepth == 24 ? 1 : 2;
    const char* depths[] = {"16-bit PCM", "24-bit PCM", "32-bit float"};
    ImGui::SetNextItemWidth(half);
    if (ImGui::Combo("##fmt", &depthIdx, depths, 3)) s.recorder.bitDepth = depthIdx == 0 ? 16 : depthIdx == 1 ? 24 : 32;
    ImGui::EndGroup();
    ImGui::SameLine(0, 10.0f * sc);
    ImGui::SetCursorPosY(rowY);
    ImGui::BeginGroup();
    pop::Label("Sample rate");
    const int rates[] = {0, 44100, 48000, 96000};
    const char* rateNames[] = {"Same as Echo", "44.1 kHz", "48 kHz", "96 kHz"};
    int ri = 0;
    for (int i = 0; i < 4; ++i)
        if (rates[i] == s.recorder.sampleRate) ri = i;
    ImGui::SetNextItemWidth(half);
    if (ImGui::Combo("##rate", &ri, rateNames, 4)) s.recorder.sampleRate = rates[ri];
    ImGui::EndGroup();
    pop::Label("Save to");
    ImGui::PushFont(fonts().regular, 13.0f);
    TextEllipsis(s.recorder.folder, ImGui::GetContentRegionAvail().x);
    ImGui::PopFont();
    if (pop::SmallButton("Change folder")) {
        std::string f = s.recorder.folder;
        if (browseFolder(hwnd_, f)) s.recorder.folder = f;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (pop::SmallButton("Open folder")) {
        CreateDirectoryW(utf8ToWide(s.recorder.folder).c_str(), nullptr);
        ShellExecuteW(nullptr, L"open", utf8ToWide(s.recorder.folder).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    pop::EndCard();

    // ------------------------------------------------------ recent recordings
    ImGui::Dummy(ImVec2(0, 4.0f * sc));
    if (active != recentWasActive_ || now_ - recentRefreshed_ > 3.0) {
        recentWasActive_ = active;
        recentRefreshed_ = now_;
        recent_ = listRecordings(utf8ToWide(s.recorder.folder), 12);
    }
    pop::BeginCard("##recent");
    pop::CardTitle("Recent recordings");
    if (recent_.empty()) pop::Hint("No recordings in this folder yet.");
    for (size_t i = 0; i < recent_.size(); ++i) {
        const RecordingFile& r = recent_[i];
        ImGui::PushID(static_cast<int>(i));
        pop::Divider(4.0f, 6.0f);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float d = 36.0f * sc;
        if (ImGui::InvisibleButton("##play", ImVec2(d, d)))
            ShellExecuteW(nullptr, L"open", r.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        Tip("Play in your default audio player");
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 c(pos.x + d * 0.5f, pos.y + d * 0.5f);
        dl->AddCircleFilled(c, d * 0.5f, ImGui::IsItemHovered() ? p.frameHover : p.surface, 32);
        dl->AddCircle(c, d * 0.5f, p.outline, 32, 1.5f * sc);
        dl->AddTriangleFilled(ImVec2(c.x - 4 * sc, c.y - 7 * sc), ImVec2(c.x - 4 * sc, c.y + 7 * sc), ImVec2(c.x + 7 * sc, c.y), p.text);
        ImGui::SameLine(0, 12.0f * sc);
        ImGui::BeginGroup();
        ImGui::PushFont(fonts().semibold, 14.0f);
        TextEllipsis(r.name, ImGui::GetContentRegionAvail().x - 130.0f * sc);
        ImGui::PopFont();
        std::time_t t = static_cast<std::time_t>(r.modified);
        std::tm tmv{};
        localtime_s(&tmv, &t);
        char when[32];
        std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tmv);
        ImGui::PushFont(fonts().regular, 12.0f);
        ImGui::TextColored(toVec4(p.text2), "%s  ·  %s  ·  %s", when, r.seconds > 0 ? formatDuration(r.seconds).c_str() : "?",
                           formatBytes(r.bytes).c_str());
        ImGui::PopFont();
        ImGui::EndGroup();
        ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 120.0f * sc);
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - 120.0f * sc,
                                         pos.y + 2.0f * sc));
        if (pop::SmallButton("Show in folder", pop::Btn::Quiet)) {
            const std::wstring args = L"/select,\"" + r.path + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        }
        ImGui::PopID();
    }
    pop::EndCard();
}

} // namespace echo::ui
