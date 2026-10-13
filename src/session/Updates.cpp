#include "session/Updates.h"

#include <algorithm>
#include <cctype>
#include <sstream>

#include "nlohmann/json.hpp"

namespace echo::updates {

Version parseVersion(std::string_view text) {
    Version v;
    size_t i = 0;
    if (i < text.size() && (text[i] == 'v' || text[i] == 'V')) ++i;
    int* parts[3] = {&v.major, &v.minor, &v.patch};
    for (int k = 0; k < 3; ++k) {
        if (i >= text.size() || !std::isdigit(static_cast<unsigned char>(text[i]))) return Version{};
        int n = 0;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])) && n < 100000) n = n * 10 + (text[i++] - '0');
        *parts[k] = n;
        if (k < 2) {
            if (i >= text.size() || text[i] != '.') return Version{};
            ++i;
        }
    }
    v.valid = true;
    return v;
}

int compare(const Version& a, const Version& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

namespace {
bool endsWithNoCase(const std::string& s, const std::string& end) {
    if (s.size() < end.size()) return false;
    return std::equal(end.rbegin(), end.rend(), s.rbegin(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
}
} // namespace

bool newerRelease(const std::string& releasesJson, const std::string& current, Release& out, std::string* error) {
    using nlohmann::json;
    json list;
    try {
        list = json::parse(releasesJson);
    } catch (const std::exception&) {
        if (error) *error = "GitHub's answer couldn't be read";
        return false;
    }
    if (!list.is_array()) {
        if (error) *error = list.is_object() && list.contains("message") ? "GitHub: " + list.value("message", std::string())
                                                                         : "GitHub's answer couldn't be read";
        return false;
    }
    const Version mine = parseVersion(current);
    const json* best = nullptr;
    Version bestVersion = mine;
    for (const auto& r : list) {
        if (!r.is_object() || r.value("draft", false)) continue;
        const Version v = parseVersion(r.value("tag_name", std::string()));
        if (v.valid && compare(v, bestVersion) > 0) {
            best = &r;
            bestVersion = v;
        }
    }
    if (error) error->clear();
    if (!best) return false;
    const json& r = *best;
    out = Release{};
    out.tag = r.value("tag_name", std::string());
    out.version = std::to_string(bestVersion.major) + "." + std::to_string(bestVersion.minor) + "." + std::to_string(bestVersion.patch);
    out.name = r.value("name", std::string());
    if (r.contains("body") && r["body"].is_string()) out.notes = r["body"].get<std::string>();
    out.pageUrl = r.value("html_url", std::string());
    out.prerelease = r.value("prerelease", false);
    if (r.contains("assets") && r["assets"].is_array()) {
        for (const auto& a : r["assets"]) {
            const std::string name = a.value("name", std::string());
            if (out.installerUrl.empty() && endsWithNoCase(name, "-setup-win64.exe")) {
                out.installerName = name;
                out.installerUrl = a.value("browser_download_url", std::string());
                out.installerBytes = a.value("size", uint64_t{0});
            } else if (endsWithNoCase(name, "SHA256SUMS.txt")) {
                out.sumsUrl = a.value("browser_download_url", std::string());
            }
        }
    }
    return true;
}

std::string checksumFor(const std::string& sums, const std::string& file) {
    std::istringstream in(sums);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const size_t space = line.find_first_of(" \t");
        if (space != 64) continue;
        std::string name = line.substr(space);
        name.erase(0, name.find_first_not_of(" \t*")); // "  name" or " *name"
        if (name != file) continue;
        std::string hash = line.substr(0, 64);
        std::transform(hash.begin(), hash.end(), hash.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (std::all_of(hash.begin(), hash.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }))
            return hash;
    }
    return {};
}

} // namespace echo::updates
