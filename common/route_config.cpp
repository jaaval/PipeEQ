#include "route_config.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace eqcore {

void to_json(nlohmann::json& j, const EqBand& b) {
    j = nlohmann::json{
        {"type", b.type},
        {"freq_hz", b.freqHz},
        {"gain_db", b.gainDb},
        {"q", b.q},
    };
}

void from_json(const nlohmann::json& j, EqBand& b) {
    j.at("type").get_to(b.type);
    j.at("freq_hz").get_to(b.freqHz);
    j.at("gain_db").get_to(b.gainDb);
    j.at("q").get_to(b.q);
}

void to_json(nlohmann::json& j, const InputConfig& i) {
    j = nlohmann::json{
        {"id", i.id},
        {"display_name", i.displayName},
    };
}

void from_json(const nlohmann::json& j, InputConfig& i) {
    j.at("id").get_to(i.id);
    j.at("display_name").get_to(i.displayName);
}

void to_json(nlohmann::json& j, const RouteConfig& r) {
    j = nlohmann::json{
        {"id", r.id},
        {"device_name", r.deviceName},
        {"display_name", r.displayName},
        {"gain_db", r.gainDb},
        {"muted", r.muted},
        {"bands", r.bands},
        {"input_gains_db", r.inputGainsDb},
    };
}

void from_json(const nlohmann::json& j, RouteConfig& r) {
    j.at("id").get_to(r.id);
    j.at("device_name").get_to(r.deviceName);
    j.at("display_name").get_to(r.displayName);
    j.at("gain_db").get_to(r.gainDb);
    j.at("muted").get_to(r.muted);
    j.at("bands").get_to(r.bands);
    // Added after the initial release - absent in configs saved before the
    // mixer feature existed, so read it leniently rather than with at().
    r.inputGainsDb = j.value("input_gains_db", std::map<std::string, double>{});
}

void to_json(nlohmann::json& j, const AppConfig& c) {
    j = nlohmann::json{{"inputs", c.inputs}, {"routes", c.routes}};
}

void from_json(const nlohmann::json& j, AppConfig& c) {
    // Same leniency as RouteConfig::inputGainsDb above: configs saved
    // before the mixer feature have no "inputs" key at all. loadConfig()'s
    // catch-all below would otherwise turn a strict j.at() miss here into
    // "silently start with zero routes," destroying an existing user's
    // saved routes/EQ on upgrade - the whole point of this being lenient.
    c.inputs = j.value("inputs", std::vector<InputConfig>{});
    j.at("routes").get_to(c.routes);
}

std::string configFilePath() {
    if (const char* xdgConfigHome = std::getenv("XDG_CONFIG_HOME")) {
        return std::filesystem::path(xdgConfigHome) / "pipeeq" / "config.json";
    }
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home ? home : ".") / ".config" / "pipeeq" / "config.json";
}

AppConfig loadConfig() {
    const std::string path = configFilePath();
    std::ifstream in(path);
    if (!in.is_open()) {
        return AppConfig{};
    }

    try {
        nlohmann::json j;
        in >> j;
        return j.get<AppConfig>();
    } catch (const std::exception& e) {
        std::cerr << "pipeeq: failed to parse config at " << path << ": " << e.what()
                  << " (starting with an empty configuration)\n";
        return AppConfig{};
    }
}

void saveConfig(const AppConfig& config) {
    const std::string path = configFilePath();
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());

    std::ofstream out(path);
    if (!out.is_open()) {
        std::cerr << "pipeeq: failed to write config at " << path << "\n";
        return;
    }

    nlohmann::json j = config;
    out << j.dump(2);
}

} // namespace eqcore
