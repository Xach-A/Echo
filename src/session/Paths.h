// Per-user file locations. Nothing is written outside these folders and the
// user-chosen recording folder.
#pragma once

#include <string>

namespace echo::paths {

std::wstring appDataDir();        // %APPDATA%\Echo            (settings, presets, models)
std::wstring localDataDir();      // %LOCALAPPDATA%\Echo       (logs, crash marker)
std::wstring sessionFile();       // appDataDir\session.json
std::wstring presetsDir();        // appDataDir\presets
std::wstring modelsDir();         // appDataDir\models
std::wstring logFile();           // localDataDir\logs\echo.log
std::wstring crashMarkerFile();   // localDataDir\running.marker
std::wstring imguiIniFile();      // appDataDir\layout.ini
std::wstring defaultRecordingsDir(); // Music\Echo Recordings
std::wstring executableDir();

// Allows tests and portable mode to redirect everything to one folder.
void overrideRoot(const std::wstring& root);

} // namespace echo::paths
