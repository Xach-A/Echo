#include "audio/Download.h"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <cstdio>
#include <filesystem>
#include <vector>

#include "Version.h"
#include "core/Text.h"

namespace echo::win {

namespace {

class Sha256 {
public:
    Sha256() {
        if (BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0)
            ok_ = BCryptCreateHash(alg_, &hash_, nullptr, 0, nullptr, 0, 0) == 0;
    }
    ~Sha256() {
        if (hash_) BCryptDestroyHash(hash_);
        if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
    }
    void add(const void* p, size_t n) {
        if (ok_) ok_ = BCryptHashData(hash_, static_cast<PUCHAR>(const_cast<void*>(p)), static_cast<ULONG>(n), 0) == 0;
    }
    std::string hex() {
        unsigned char d[32];
        if (!ok_ || BCryptFinishHash(hash_, d, sizeof(d), 0) != 0) return {};
        ok_ = false;
        char buf[65];
        for (int i = 0; i < 32; ++i) std::snprintf(buf + 2 * i, 3, "%02x", d[i]);
        return buf;
    }

private:
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    bool ok_ = false;
};

struct Handle {
    HINTERNET h = nullptr;
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
};

} // namespace

std::string sha256File(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return {};
    Sha256 sha;
    std::vector<char> buf(1 << 20);
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) sha.add(buf.data(), n);
    std::fclose(f);
    return sha.hex();
}

namespace {

// Sends an HTTPS GET (redirects followed) and waits for the answer's headers.
bool openGet(const std::string& url, const wchar_t* headers, Handle& session, Handle& connect, Handle& request, DWORD& status,
             std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    const std::wstring wurl = utf8ToWide(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2047;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) return fail("Bad download address");
    session.h = WinHttpOpen(L"Echo/" ECHO_VERSION_WSTRING, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) return fail("Can't start the download");
    connect.h = WinHttpConnect(session.h, host, uc.nPort, 0);
    request.h = connect.h ? WinHttpOpenRequest(connect.h, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               WINHTTP_FLAG_SECURE)
                          : nullptr;
    if (!request.h ||
        !WinHttpSendRequest(request.h, headers ? headers : WINHTTP_NO_ADDITIONAL_HEADERS, headers ? static_cast<DWORD>(-1) : 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr))
        return fail("Can't reach " + wideToUtf8(host) + " (check your internet connection)");
    status = 0;
    DWORD len = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                        WINHTTP_NO_HEADER_INDEX);
    return true;
}

} // namespace

bool httpGetText(const std::string& url, const wchar_t* headers, std::string& out, size_t maxBytes, int* httpStatus,
                 std::string* error) {
    Handle session, connect, request;
    DWORD status = 0;
    out.clear();
    if (!openGet(url, headers, session, connect, request, status, error)) return false;
    if (httpStatus) *httpStatus = static_cast<int>(status);
    std::vector<char> buf(1 << 16);
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(request.h, buf.data(), static_cast<DWORD>(buf.size()), &got)) {
            if (error) *error = "The download was interrupted";
            return false;
        }
        if (got == 0) break;
        out.append(buf.data(), got);
        if (out.size() > maxBytes) {
            if (error) *error = "The answer is larger than expected";
            return false;
        }
    }
    if (status != 200) {
        if (error) *error = "The server answered " + std::to_string(status);
        return false;
    }
    return true;
}

bool downloadFile(const std::string& url, const std::wstring& dest, uint64_t expectedSize, const std::string& sha256Hex,
                  DownloadProgress& progress, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    Handle session, connect, request;
    DWORD status = 0;
    if (!openGet(url, nullptr, session, connect, request, status, error)) return false;
    if (status != 200) return fail("The server answered " + std::to_string(status));
    progress.total = expectedSize;
    progress.done = 0;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(dest).parent_path(), ec);
    const std::wstring part = dest + L".part";
    FILE* f = _wfopen(part.c_str(), L"wb");
    if (!f) return fail("Can't write to " + wideToUtf8(part));
    Sha256 sha;
    std::vector<char> buf(1 << 16);
    uint64_t total = 0;
    bool ok = true;
    for (;;) {
        if (progress.cancel.load()) {
            ok = fail("Cancelled");
            break;
        }
        DWORD got = 0;
        if (!WinHttpReadData(request.h, buf.data(), static_cast<DWORD>(buf.size()), &got)) {
            ok = fail("The download was interrupted");
            break;
        }
        if (got == 0) break;
        if (std::fwrite(buf.data(), 1, got, f) != got) {
            ok = fail("Not enough disk space");
            break;
        }
        sha.add(buf.data(), got);
        total += got;
        progress.done = total;
        if (total > expectedSize) {
            ok = fail("The file is larger than expected");
            break;
        }
    }
    std::fclose(f);
    if (ok && total != expectedSize) ok = fail("The file is incomplete");
    if (ok && sha.hex() != sha256Hex) ok = fail("The file doesn't match its checksum (not the expected file)");
    if (!ok) {
        std::filesystem::remove(part, ec);
        return false;
    }
    std::filesystem::rename(part, dest, ec);
    if (ec) {
        std::filesystem::remove(part, ec);
        return fail("Can't save " + wideToUtf8(dest));
    }
    return true;
}

} // namespace echo::win
