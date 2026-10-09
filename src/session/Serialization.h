// JSON serialisation for sessions and presets. Formats are documented in
// docs/FILE_FORMATS.md. Unknown fields are ignored and missing fields take
// defaults, so files from older or newer versions load gracefully.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "session/Session.h"

namespace echo {

using json = nlohmann::json;

json effectToJson(const Effect& e);
// Creates (unprepared) and configures an effect. Returns nullptr for an
// unknown type and appends a message to `warnings`.
std::shared_ptr<Effect> effectFromJson(const json& j, std::vector<std::string>* warnings);

json chainToJson(const std::vector<std::shared_ptr<Effect>>& chain);
std::vector<std::shared_ptr<Effect>> chainFromJson(const json& j, std::vector<std::string>* warnings);

json sessionToJson(const Session& s);
// Builds a session (runtime objects get their parameters set). Returns false
// only if the document is not an Echo session at all.
bool sessionFromJson(const json& j, Session& out, std::vector<std::string>* warnings);

// Atomic save: writes "<path>.tmp", keeps the previous file as "<path>.bak",
// then renames. Loading falls back to the .bak if the main file is corrupt.
bool saveJsonFile(const std::wstring& path, const json& j, std::string* error);
bool loadJsonFile(const std::wstring& path, json& out, std::string* error, bool tryBackup = true);

} // namespace echo
