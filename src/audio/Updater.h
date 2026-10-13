// Updates from Echo's GitHub releases, only when the user asks (Settings >
// About > Check for updates): which release is newer, downloading its
// installer (checked against the release's SHA256SUMS.txt) and starting it
// quietly; the installer closes Echo, updates it and starts it again.
#pragma once

#include <string>

#include "audio/Download.h"
#include "session/Updates.h"

namespace echo::win {

enum class UpdateCheck { UpToDate, Available, Failed };

// Asks GitHub for the releases and compares them with `current`.
UpdateCheck checkForUpdate(const std::string& current, updates::Release& out, std::string* error);

// How this Echo.exe was installed: by Echo's installer for this user or for
// all users (then the installer can update it in place), or neither
// (a portable or self-built copy).
enum class InstallKind { NotInstalled, CurrentUser, AllUsers };
InstallKind installKind();

// Downloads the release's installer into `folder`, checked against its size
// and the SHA-256 in the release's SHA256SUMS.txt. Blocks.
bool downloadUpdate(const updates::Release& r, const std::wstring& folder, DownloadProgress& progress, std::wstring* installer,
                    std::string* error);

// Starts the downloaded installer quietly for this kind of install. The
// caller quits Echo right after; the installer starts the new version.
bool startUpdate(const std::wstring& installer, InstallKind kind, std::string* error);

} // namespace echo::win
