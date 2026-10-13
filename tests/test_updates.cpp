// Updates: comparing versions, picking the newest release from GitHub's
// list, and reading SHA256SUMS.txt. (Asking GitHub for real:
// `echo_hwtest update-check`.)
#include "doctest/doctest.h"

#include <string>

#include "session/Updates.h"

using namespace echo;

TEST_CASE("updates: versions are read and compared") {
    const auto v = updates::parseVersion("v0.1.2");
    CHECK(v.valid);
    CHECK(v.major == 0);
    CHECK(v.minor == 1);
    CHECK(v.patch == 2);
    CHECK(updates::parseVersion("1.10.0-beta").valid);
    CHECK_FALSE(updates::parseVersion("latest").valid);
    CHECK_FALSE(updates::parseVersion("v1.2").valid);
    CHECK(updates::compare(updates::parseVersion("0.1.10"), updates::parseVersion("0.1.9")) > 0);
    CHECK(updates::compare(updates::parseVersion("1.0.0"), updates::parseVersion("0.9.9")) > 0);
    CHECK(updates::compare(updates::parseVersion("v0.2.0"), updates::parseVersion("0.2.0")) == 0);
    CHECK(updates::compare(updates::parseVersion("0.1.0"), updates::parseVersion("0.1.1")) < 0);
}

TEST_CASE("updates: the newest published release newer than this Echo is picked, with its installer") {
    // As GitHub lists releases (newest first, but not necessarily by version).
    const std::string json = R"([
      {"tag_name": "v0.3.0", "draft": true, "prerelease": false, "assets": []},
      {"tag_name": "v0.2.0", "name": "Echo 0.2.0", "draft": false, "prerelease": true, "body": "**New**\n- things",
       "html_url": "https://github.com/x/y/releases/tag/v0.2.0",
       "assets": [
         {"name": "Echo-0.2.0-portable-win64.zip", "size": 10, "browser_download_url": "https://example.com/zip"},
         {"name": "Echo-0.2.0-setup-win64.exe", "size": 9000000, "browser_download_url": "https://example.com/setup"},
         {"name": "SHA256SUMS.txt", "size": 191, "browser_download_url": "https://example.com/sums"}]},
      {"tag_name": "v0.1.5", "draft": false, "prerelease": false, "assets": []},
      {"tag_name": "nightly", "draft": false, "prerelease": true, "assets": []}
    ])";
    updates::Release r;
    std::string err;
    REQUIRE(updates::newerRelease(json, "0.1.0", r, &err));
    CHECK(r.version == "0.2.0"); // the draft 0.3.0 is skipped, the pre-release counts
    CHECK(r.prerelease);
    CHECK(r.installerName == "Echo-0.2.0-setup-win64.exe");
    CHECK(r.installerUrl == "https://example.com/setup");
    CHECK(r.installerBytes == 9000000u);
    CHECK(r.sumsUrl == "https://example.com/sums");
    CHECK(r.pageUrl == "https://github.com/x/y/releases/tag/v0.2.0");
    CHECK(r.notes.find("things") != std::string::npos);
    // Up to date: nothing newer, and no error.
    CHECK_FALSE(updates::newerRelease(json, "0.2.0", r, &err));
    CHECK(err.empty());
    CHECK_FALSE(updates::newerRelease("[]", "0.1.0", r, &err));
    CHECK(err.empty());
    // An error from GitHub, or not JSON at all.
    CHECK_FALSE(updates::newerRelease(R"({"message": "API rate limit exceeded"})", "0.1.0", r, &err));
    CHECK(err.find("rate limit") != std::string::npos);
    CHECK_FALSE(updates::newerRelease("<html>", "0.1.0", r, &err));
    CHECK_FALSE(err.empty());
}

TEST_CASE("updates: the installer's checksum is read from SHA256SUMS.txt") {
    const std::string sums =
        "EA8D4A977D91DCACA3D971CD16A3D6BFA398B856C0DA264DE6382CD6F94BBC7D  Echo-0.1.0-portable-win64.zip\r\n"
        "c3538a27d5ec62b6e836dca292e9b9ea207bd7351307ceb520093fb51f0013f8 *Echo-0.1.0-setup-win64.exe\r\n";
    CHECK(updates::checksumFor(sums, "Echo-0.1.0-setup-win64.exe") ==
          "c3538a27d5ec62b6e836dca292e9b9ea207bd7351307ceb520093fb51f0013f8");
    CHECK(updates::checksumFor(sums, "Echo-0.1.0-portable-win64.zip") ==
          "ea8d4a977d91dcaca3d971cd16a3d6bfa398b856c0da264de6382cd6f94bbc7d");
    CHECK(updates::checksumFor(sums, "Echo-0.1.0-setup-win64").empty());
    CHECK(updates::checksumFor("not a list", "x").empty());
}
