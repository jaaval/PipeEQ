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
        {"auto_connect", r.autoConnect},
        {"channels", nlohmann::json::array({r.leftChannel, r.rightChannel})},
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
    // Likewise added later; configs predating it get the default-on
    // behavior, which is what they effectively had already.
    r.autoConnect = j.value("auto_connect", true);
    // Also added later. Absent, or anything other than exactly two names,
    // means "device default" - the pre-channel-pair behavior.
    const auto channels = j.value("channels", std::vector<std::string>{});
    if (channels.size() == 2) {
        r.leftChannel = channels[0];
        r.rightChannel = channels[1];
    }
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

    // Write-then-rename rather than truncating the real file in place: this
    // is rewritten on every slider move, so a crash or full disk midway
    // through would otherwise leave a half-written config that parses as
    // "no routes at all" and loses every saved EQ curve.
    const std::string tempPath = path + ".tmp";
    {
        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            std::cerr << "pipeeq: failed to write config at " << tempPath << "\n";
            return;
        }

        nlohmann::json j = config;
        out << j.dump(2) << '\n';
        out.flush();
        if (!out.good()) {
            std::cerr << "pipeeq: failed to write config at " << tempPath << "\n";
            out.close();
            std::error_code ignored;
            std::filesystem::remove(tempPath, ignored);
            return;
        }
    }

    std::error_code ec;
    std::filesystem::rename(tempPath, path, ec);
    if (ec) {
        std::cerr << "pipeeq: failed to replace config at " << path << ": " << ec.message() << "\n";
        std::error_code ignored;
        std::filesystem::remove(tempPath, ignored);
    }
}

} // namespace eqcore
