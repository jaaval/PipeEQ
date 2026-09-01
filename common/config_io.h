#pragma once

#include <string>
#include <vector>

#include "app_config.h"

namespace eqcore {

enum class LoadStatus {
    Missing,  // no config file yet: a first run, not an error
    Loaded,   // parsed as the current version
    Migrated, // parsed as an older version and converted
    Failed,   // unreadable, unparseable, or from a newer version
};

struct LoadResult {
    AppConfig config;
    LoadStatus status = LoadStatus::Missing;
    std::string message;               // human-readable detail for Failed
    std::string path;                  // the file that was (or wasn't) read
    std::vector<std::string> warnings; // per-field repairs, worth logging

    // Whether it is safe to write this config back over the file it came from.
    //
    // This is the load-bearing part. Previously a parse failure returned an
    // empty config, and the next mutating D-Bus call happily wrote that empty
    // config over a perfectly good file - so one malformed byte silently
    // destroyed every saved output and EQ curve. Failed loads must never be
    // persisted; the daemon runs read-only for the session instead.
    bool persistable() const { return status != LoadStatus::Failed; }
};

// Full path to the config file: $XDG_CONFIG_HOME/pipeeq/config.json, falling
// back to ~/.config/pipeeq/config.json.
std::string configFilePath();

// Reads and parses the config, migrating a v1 document if it finds one. Never
// throws. A Migrated result has already had the original copied aside to
// "<path>.v1.bak" - and if that backup can't be written the load is reported as
// Failed rather than risking an unreversible overwrite.
LoadResult loadConfig();

// Writes to configFilePath(), creating parent directories as needed. Returns
// false (and logs) on failure.
//
// Durable: writes a temp file, fsyncs it, renames over the target, then fsyncs
// the parent directory. A plain flush()+rename can leave a zero-length config
// after a power loss, which is exactly the outcome the write-then-rename was
// meant to prevent.
bool saveConfig(const AppConfig& config);

} // namespace eqcore
