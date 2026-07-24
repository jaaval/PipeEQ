#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "eq_band.h"

namespace eqcore {

NLOHMANN_JSON_SERIALIZE_ENUM(FilterType, {
    {FilterType::Peaking, "peaking"},
    {FilterType::LowShelf, "low_shelf"},
    {FilterType::HighShelf, "high_shelf"},
    {FilterType::LowPass, "low_pass"},
    {FilterType::HighPass, "high_pass"},
})

void to_json(nlohmann::json& j, const EqBand& b);
void from_json(const nlohmann::json& j, EqBand& b);

// A single output route: one physical device with its own gain/mute/EQ.
struct RouteConfig {
    std::string id;          // stable route id (e.g. "route-1"), assigned by the daemon
    std::string deviceName;  // PipeWire node.name of the target physical sink
    std::string displayName; // user-facing label, defaults to deviceName
    double gainDb = 0.0;
    bool muted = false;
    std::vector<EqBand> bands;
};

void to_json(nlohmann::json& j, const RouteConfig& r);
void from_json(const nlohmann::json& j, RouteConfig& r);

struct AppConfig {
    std::vector<RouteConfig> routes;
};

void to_json(nlohmann::json& j, const AppConfig& c);
void from_json(const nlohmann::json& j, AppConfig& c);

// Full path to the config file: $XDG_CONFIG_HOME/pipeeq/config.json,
// falling back to ~/.config/pipeeq/config.json.
std::string configFilePath();

// Returns a default-constructed AppConfig (no routes) if the file doesn't exist
// or fails to parse.
AppConfig loadConfig();

// Writes to configFilePath(), creating parent directories as needed.
void saveConfig(const AppConfig& config);

} // namespace eqcore
