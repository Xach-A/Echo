// Updates: which GitHub release is newer than this Echo, and the checksum of
// its installer. Pure logic; asking GitHub and installing are in
// audio/Updater.h, and happen only when the user clicks.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace echo::updates {

struct Version {
    int major = 0, minor = 0, patch = 0;
    bool valid = false;
};

// "v0.1.2", "0.1.2" or "0.1.2-beta" (anything after the numbers is ignored).
Version parseVersion(std::string_view text);
// < 0, 0, > 0 like strcmp.
int compare(const Version& a, const Version& b);

struct Release {
    std::string tag, version, name, notes, pageUrl;
    std::string installerName, installerUrl, sumsUrl;
    uint64_t installerBytes = 0;
    bool prerelease = false;
};

// From GitHub's list of releases (JSON): the newest published release (drafts
// skipped, pre-releases included) if it is newer than `current`. False when
// there is none or the list can't be read (`error` says which).
bool newerRelease(const std::string& releasesJson, const std::string& current, Release& out, std::string* error);

// The SHA-256 listed for `file` in a SHA256SUMS.txt ("<hash>  <file>" lines),
// lowercase; "" if it isn't listed.
std::string checksumFor(const std::string& sums, const std::string& file);

} // namespace echo::updates
