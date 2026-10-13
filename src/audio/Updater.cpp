#include "audio/Updater.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <filesystem>
#include <initializer_list>

#include "Version.h"
#include "core/Text.h"

namespace echo::win {

namespace {
// The installer's uninstall entry ("<AppId>_is1"; the AppId is in
// installer/Echo.iss) records where it put Echo.
constexpr const wchar_t* kUninstallKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{CAB4B4EF-1163-4A4A-9293-11E8A2D26499}_is1";

std::wstring normalizedDir(std::wstring d) {
    while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    std::transform(d.begin(), d.end(), d.begin(), ::towlower);
    return d;
}
} // namespace

UpdateCheck checkForUpdate(const std::string& current, updates::Release& out, std::string* error) {
    const std::string url = "https://api.github.com/repos/" ECHO_UPDATE_REPO "/releases?per_page=20";
    std::string body;
    int status = 0;
    if (!httpGetText(url, L"Accept: application/vnd.github+json\r\n", body, 4u << 20, &status, error)) {
        if (error && (status == 403 || status == 429))
            *error = "GitHub has had too many update checks from this network; try again in an hour";
        else if (error && status == 404)
            *error = "The update source (github.com/" ECHO_UPDATE_REPO ") wasn't found";
        return UpdateCheck::Failed;
    }
    std::string err;
    if (updates::newerRelease(body, current, out, &err)) return UpdateCheck::Available;
    if (!err.empty()) {
        if (error) *error = err;
        return UpdateCheck::Failed;
    }
    return UpdateCheck::UpToDate;
}

InstallKind installKind() {
    wchar_t exe[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    const std::wstring here = normalizedDir(std::filesystem::path(std::wstring(exe, n)).parent_path().wstring());
    for (const auto& [root, kind] : {std::pair{HKEY_CURRENT_USER, InstallKind::CurrentUser}, std::pair{HKEY_LOCAL_MACHINE, InstallKind::AllUsers}}) {
        wchar_t buf[MAX_PATH * 2];
        DWORD size = sizeof(buf);
        if (RegGetValueW(root, kUninstallKey, L"InstallLocation", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS &&
            normalizedDir(buf) == here)
            return kind;
    }
    return InstallKind::NotInstalled;
}

bool downloadUpdate(const updates::Release& r, const std::wstring& folder, DownloadProgress& progress, std::wstring* installer,
                    std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (r.installerUrl.empty() || r.sumsUrl.empty() || r.installerBytes == 0)
        return fail("This release has no installer and checksums to update from; download it from the release page");
    // The file name comes from GitHub: keep it to a plain name.
    if (r.installerName.find_first_of("\\/:") != std::string::npos || r.installerName.find("..") != std::string::npos)
        return fail("The release's installer has an unexpected name");
    std::string sums;
    int status = 0;
    if (!httpGetText(r.sumsUrl, nullptr, sums, 64 * 1024, &status, error)) return false;
    const std::string sha = updates::checksumFor(sums, r.installerName);
    if (sha.empty()) return fail("The release's SHA256SUMS.txt doesn't list " + r.installerName);
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const std::wstring dest = folder + L"\\" + utf8ToWide(r.installerName);
    if (sha256File(dest) != sha) {
        std::filesystem::remove(dest, ec);
        if (!downloadFile(r.installerUrl, dest, r.installerBytes, sha, progress, error)) return false;
    } else {
        progress.total = r.installerBytes;
        progress.done = r.installerBytes;
    }
    if (installer) *installer = dest;
    return true;
}

bool startUpdate(const std::wstring& installer, InstallKind kind, std::string* error) {
    // /update=1 makes the installer start Echo again when it's done.
    const std::wstring params = std::wstring(L"/SILENT /SUPPRESSMSGBOXES /NORESTART /update=1 ") +
                                (kind == InstallKind::AllUsers ? L"/ALLUSERS" : L"/CURRENTUSER");
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"open";
    sei.lpFile = installer.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow = SW_SHOWNORMAL;
    const bool ok = ShellExecuteExW(&sei) != FALSE;
    const DWORD err = ok ? 0 : GetLastError();
    if (SUCCEEDED(co)) CoUninitialize();
    if (!ok && error) *error = "Couldn't start the installer (error " + std::to_string(err) + ")";
    return ok;
}

} // namespace echo::win
