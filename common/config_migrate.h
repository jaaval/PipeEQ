#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "app_config.h"

namespace eqcore {

// Converts a v1 config document into a v2 AppConfig.
//
// Pure: takes JSON, returns structs, touches no files - which is what makes it
// unit-testable against the exact v1 documents users actually have.
//
// Each v1 "route" (one device + one stereo pair, one flat band list shared by
// both channels) becomes one v2 "output" with two channels linked as a group,
// its bands becoming a single EQ instance referenced by both. The result
// therefore behaves exactly like the v1 route did: one fader, one mute, one
// EQ, one set of sends.
//
// Returns nullopt only if the document isn't a v1 document at all. A missing or
// malformed "routes" array yields an empty-but-valid config plus a warning
// rather than a failure: v1's parser treated that as a hard error, and the
// caller turned the error into "zero routes" and then saved it.
std::optional<AppConfig> migrateV1(const nlohmann::json& j, std::vector<std::string>& warnings);

} // namespace eqcore
