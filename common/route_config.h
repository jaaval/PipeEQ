#pragma once

#include <map>
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

// One audio input (today always a virtual sink apps can be assigned to).
struct InputConfig {
    std::string id;          // stable id (e.g. "input-1"), assigned by the daemon
    std::string displayName; // user-facing label
};

void to_json(nlohmann::json& j, const InputConfig& i);
void from_json(const nlohmann::json& j, InputConfig& i);

// A single output route: one physical device with its own gain/mute/EQ,
// plus its mix level for each input it hears (input id -> gain in dB;
// inputs not present here are silent on this route).
//
// This is the *desired* configuration and the daemon's single source of
// truth for a route, whether or not the target device happens to be
// present right now. A route whose device is missing stays here in full
// (see AudioEngine::RouteEntry) so that unplugging a device can never
// erase its EQ, and so it can be reconnected the moment the device
// reappears.
struct RouteConfig {
    std::string id;          // stable route id (e.g. "route-1"), assigned by the daemon
    std::string deviceName;  // PipeWire node.name of the target physical sink
    std::string displayName; // user-facing label, defaults to deviceName
    // Which stereo pair of the target device this output drives, as SPA
    // channel short names ("FL"/"FR", "RL"/"RR", ...). This is what lets one
    // multi-channel interface host several independent outputs - a Scarlett
    // 4i4's outputs 1/2 and 3/4 are two routes, each with its own EQ. Empty
    // means "whatever the device does by default", the behavior for devices
    // with no layout PipeEQ recognizes.
    std::string leftChannel;
    std::string rightChannel;
    double gainDb = 0.0;
    bool muted = false;
    std::vector<EqBand> bands;
    std::map<std::string, double> inputGainsDb;
    // When true (the default), the daemon connects this route as soon as
    // its device exists - at startup if it's already there, or later
    // whenever it appears - and reconnects it after a replug.
    bool autoConnect = true;
};

void to_json(nlohmann::json& j, const RouteConfig& r);
void from_json(const nlohmann::json& j, RouteConfig& r);

struct AppConfig {
    std::vector<InputConfig> inputs;
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
