#include "audio/VbCable.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <filesystem>

namespace echo::win {

bool unzip(const std::wstring& zip, const std::wstring& dir, std::string* error) {
    wchar_t sys[MAX_PATH];
    const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    const std::wstring tar = std::wstring(sys, n) + L"\\tar.exe";
    std::error_code ec;
    if (n == 0 || !std::filesystem::exists(tar, ec)) {
        if (error) *error = "Windows' tar.exe is missing, so the download can't be unpacked.";
        return false;
    }
    std::filesystem::create_directories(dir, ec);
    std::wstring cmd = L"\"" + tar + L"\" -xf \"" + zip + L"\" -C \"" + dir + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(tar.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        if (error) *error = "Couldn't unpack the download (error " + std::to_string(GetLastError()) + ").";
        return false;
    }
    const DWORD waited = WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 1;
    if (waited == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (code != 0) {
        if (error) *error = "Couldn't unpack the download.";
        return false;
    }
    return true;
}

bool prepareVbCable(const std::wstring& folder, DownloadProgress& progress, std::wstring* setupExe, std::string* error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(folder, ec);
    const std::wstring zip = folder + L"\\VBCABLE_Driver_Pack45.zip";
    if (sha256File(zip) == kVbCableSha256) {
        progress.total = kVbCableBytes;
        progress.done = kVbCableBytes;
    } else {
        fs::remove(zip, ec);
        if (!downloadFile(kVbCableUrl, zip, kVbCableBytes, kVbCableSha256, progress, error)) return false;
    }
    const std::wstring pack = folder + L"\\pack";
    fs::remove_all(pack, ec);
    if (!unzip(zip, pack, error)) return false;
    // Echo is 64-bit, so Windows is too (x64 or ARM64, which the x64 setup also handles).
    BOOL wow = FALSE;
    const bool os64 = sizeof(void*) == 8 || (IsWow64Process(GetCurrentProcess(), &wow) && wow);
    const std::wstring exe = pack + (os64 ? L"\\VBCABLE_Setup_x64.exe" : L"\\VBCABLE_Setup.exe");
    if (!fs::exists(exe, ec)) {
        if (error) *error = "VB-Audio's installer isn't in the download.";
        return false;
    }
    if (setupExe) *setupExe = exe;
    return true;
}

bool openVbCableSetup(const std::wstring& setupExe, std::string* error) {
    // ShellExecuteEx wants COM on the calling thread (this runs on a worker).
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const std::wstring dir = std::filesystem::path(setupExe).parent_path().wstring();
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"runas"; // VB-Audio's setup needs administrator rights; Windows asks the user
    sei.lpFile = setupExe.c_str();
    sei.lpDirectory = dir.c_str();
    sei.nShow = SW_SHOWNORMAL;
    const bool ok = ShellExecuteExW(&sei) != FALSE;
    const DWORD err = ok ? 0 : GetLastError();
    if (SUCCEEDED(co)) CoUninitialize();
    if (!ok && error) {
        *error = err == ERROR_CANCELLED ? "Windows didn't get permission, so VB-CABLE wasn't installed."
                                        : "Couldn't open VB-Audio's installer (error " + std::to_string(err) + ").";
    }
    return ok;
}

} // namespace echo::win
