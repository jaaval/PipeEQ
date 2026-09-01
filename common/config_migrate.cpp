#include "config_migrate.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <string_view>

namespace eqcore {

namespace {

template <typename T>
T readOr(const nlohmann::json& j, const char* key, T fallback) {
    if (!j.is_object()) {
        return fallback;
    }
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) {
        return fallback;
    }
    try {
        return it->get<T>();
    } catch (const nlohmann::json::exception&) {
        return fallback;
    }
}

// "route-3" -> "output-3". Anything else keeps its whole id under the new
// prefix, so a hand-edited id can't collide with a generated one.
std::string outputIdFromRouteId(const std::string& routeId) {
    static constexpr std::string_view kPrefix = "route-";
    if (routeId.size() > kPrefix.size() && routeId.compare(0, kPrefix.size(), kPrefix) == 0) {
        const std::string suffix = routeId.substr(kPrefix.size());
        if (!suffix.empty() && std::all_of(suffix.begin(), suffix.end(), [](unsigned char c) {
                return std::isdigit(c) != 0;
            })) {
            return "output-" + suffix;
        }
    }
    return "output-" + routeId;
}

std::optional<EqBand> parseV1Band(const nlohmann::json& j) {
    if (!j.is_object()) {
        return std::nullopt;
    }
    EqBand band;
    band.type = readOr(j, "type", FilterType::Peaking);
    band.freqHz = readOr(j, "freq_hz", 1000.0);
    band.gainDb = readOr(j, "gain_db", 0.0);
    band.q = readOr(j, "q", 0.707);
    return band;
}

} // namespace

std::optional<AppConfig> migrateV1(const nlohmann::json& j, std::vector<std::string>& warnings) {
    if (!j.is_object()) {
        warnings.push_back("v1 config root is not a JSON object");
        return std::nullopt;
    }

    AppConfig config;
    config.version = kConfigVersion;

    // v1 inputs were id + display_name, and always stereo.
    if (const auto it = j.find("inputs"); it != j.end() && it->is_array()) {
        for (const auto& inputJson : *it) {
            if (!inputJson.is_object()) {
                warnings.push_back("skipped a v1 input that isn't a JSON object");
                continue;
            }
            InputConfig input;
            input.id = readOr<std::string>(inputJson, "id", "");
            if (input.id.empty()) {
                warnings.push_back("skipped a v1 input with no id");
                continue;
            }
            input.displayName = readOr<std::string>(inputJson, "display_name", input.id);
            input.positions = {"FL", "FR"};
            config.inputs.push_back(std::move(input));
        }
    }

    // Configs predating the mixer feature have no inputs at all. Synthesize the
    // one default input every saved route implicitly heard, so upgrading stays
    // a no-op for those users. (This replaces the equivalent fixup that used to
    // live inline in DbusService::start().)
    const bool synthesizedDefaultInput = config.inputs.empty();
    if (synthesizedDefaultInput) {
        InputConfig defaultInput;
        defaultInput.id = "input-1";
        defaultInput.displayName = "Default";
        defaultInput.positions = {"FL", "FR"};
        config.inputs.push_back(std::move(defaultInput));
        warnings.push_back("v1 config predates the mixer feature; synthesized the 'Default' input");
    }

    const auto routes = j.find("routes");
    if (routes == j.end() || !routes->is_array()) {
        warnings.push_back("v1 config has no readable \"routes\" array; migrating an empty config");
        return config;
    }

    for (const auto& routeJson : *routes) {
        if (!routeJson.is_object()) {
            warnings.push_back("skipped a v1 route that isn't a JSON object");
            continue;
        }

        const std::string routeId = readOr<std::string>(routeJson, "id", "");
        const std::string deviceName = readOr<std::string>(routeJson, "device_name", "");
        if (routeId.empty() || deviceName.empty()) {
            warnings.push_back("skipped a v1 route with no id or no device_name");
            continue;
        }

        OutputConfig output;
        output.id = outputIdFromRouteId(routeId);
        output.deviceName = deviceName;
        output.displayName = readOr<std::string>(routeJson, "display_name", deviceName);
        output.autoConnect = readOr(routeJson, "auto_connect", true);

        // v1 pinned an output to exactly two channel positions. Absent, or
        // anything other than exactly two non-empty names, meant "device
        // default" - which in practice was the front pair, and which
        // adoptDeviceLayout() will correct against the real device anyway.
        std::string leftName = "FL";
        std::string rightName = "FR";
        const auto channels = readOr(routeJson, "channels", std::vector<std::string>{});
        if (channels.size() == 2 && !channels[0].empty() && !channels[1].empty()) {
            leftName = channels[0];
            rightName = channels[1];
        }

        // The route's flat band list becomes one EQ instance shared by both
        // channels, which is precisely v1's behaviour: one curve, both sides.
        std::string eqInstanceId;
        std::vector<EqBand> bands;
        if (const auto it = routeJson.find("bands"); it != routeJson.end() && it->is_array()) {
            for (const auto& bandJson : *it) {
                if (auto band = parseV1Band(bandJson)) {
                    bands.push_back(*band);
                } else {
                    warnings.push_back("skipped a malformed band in v1 route '" + routeId + "'");
                }
            }
        }
        if (!bands.empty()) {
            EqInstanceConfig instance;
            instance.id = "eq-1";
            instance.displayName = "Main";
            instance.bands = std::move(bands);
            eqInstanceId = instance.id;
            output.eqInstances.push_back(std::move(instance));
        }

        // Gain, mute and every send are copied to BOTH channels, which are then
        // linked - so the migrated output has one fader, one mute and one set
        // of sends, exactly as the v1 route did.
        const double gainDb = readOr(routeJson, "gain_db", 0.0);
        const bool muted = readOr(routeJson, "muted", false);
        auto sends = readOr(routeJson, "input_gains_db", std::map<std::string, double>{});
        if (synthesizedDefaultInput) {
            sends["input-1"] = 0.0;
        }

        for (const std::string& position : {leftName, rightName}) {
            OutputChannelConfig channel;
            channel.position = position;
            channel.gainDb = gainDb;
            channel.muted = muted;
            channel.eqInstanceId = eqInstanceId;
            channel.sendsDb = sends;
            output.channels.push_back(std::move(channel));
        }

        LinkGroupConfig group;
        group.id = "group-1";
        group.displayName = leftName + "/" + rightName;
        group.channelIndices = {0, 1};
        output.linkGroups.push_back(std::move(group));

        config.outputs.push_back(std::move(output));
    }

    // Same clamping and pruning a v2 document gets, so a migrated config and a
    // hand-written one are treated identically from here on.
    sanitize(config, warnings);
    return config;
}

} // namespace eqcore
