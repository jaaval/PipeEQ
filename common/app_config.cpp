#include "app_config.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace eqcore {

namespace {

// Reads an optional scalar without throwing on a present-but-wrong-typed
// value. nlohmann's j.value() still throws if the key exists with an
// incompatible type, which is exactly the hand-edited-file case that must
// degrade to a default rather than abort the load.
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

std::string idWithSuffix(const std::string& prefix, std::size_t n) {
    return prefix + std::to_string(n);
}

// The numeric suffix of "prefix<N>", or 0 if it doesn't have that shape.
std::size_t suffixOf(const std::string& id, const std::string& prefix) {
    if (id.size() <= prefix.size() || id.compare(0, prefix.size(), prefix) != 0) {
        return 0;
    }
    const std::string digits = id.substr(prefix.size());
    if (digits.empty() || !std::all_of(digits.begin(), digits.end(),
                                        [](unsigned char c) { return std::isdigit(c) != 0; })) {
        return 0;
    }
    try {
        return static_cast<std::size_t>(std::stoull(digits));
    } catch (const std::exception&) {
        return 0;
    }
}

} // namespace

// ---------------------------------------------------------------- accessors --

EqInstanceConfig* OutputConfig::findEqInstance(const std::string& eqId) {
    if (eqId.empty()) {
        return nullptr;
    }
    for (auto& instance : eqInstances) {
        if (instance.id == eqId) {
            return &instance;
        }
    }
    return nullptr;
}

const EqInstanceConfig* OutputConfig::findEqInstance(const std::string& eqId) const {
    return const_cast<OutputConfig*>(this)->findEqInstance(eqId);
}

LinkGroupConfig* OutputConfig::findLinkGroup(const std::string& groupId) {
    if (groupId.empty()) {
        return nullptr;
    }
    for (auto& group : linkGroups) {
        if (group.id == groupId) {
            return &group;
        }
    }
    return nullptr;
}

const LinkGroupConfig* OutputConfig::findLinkGroup(const std::string& groupId) const {
    return const_cast<OutputConfig*>(this)->findLinkGroup(groupId);
}

const LinkGroupConfig* OutputConfig::groupOfChannel(std::size_t channelIndex) const {
    for (const auto& group : linkGroups) {
        for (uint32_t member : group.channelIndices) {
            if (static_cast<std::size_t>(member) == channelIndex) {
                return &group;
            }
        }
    }
    return nullptr;
}

std::vector<std::size_t> OutputConfig::linkedChannels(std::size_t channelIndex) const {
    if (const LinkGroupConfig* group = groupOfChannel(channelIndex)) {
        std::vector<std::size_t> members;
        members.reserve(group->channelIndices.size());
        for (uint32_t member : group->channelIndices) {
            if (static_cast<std::size_t>(member) < channels.size()) {
                members.push_back(static_cast<std::size_t>(member));
            }
        }
        if (!members.empty()) {
            return members;
        }
    }
    return {channelIndex};
}

std::string OutputConfig::nextEqInstanceId() const {
    std::size_t highest = 0;
    for (const auto& instance : eqInstances) {
        highest = std::max(highest, suffixOf(instance.id, "eq-"));
    }
    return idWithSuffix("eq-", highest + 1);
}

std::string OutputConfig::nextLinkGroupId() const {
    std::size_t highest = 0;
    for (const auto& group : linkGroups) {
        highest = std::max(highest, suffixOf(group.id, "group-"));
    }
    return idWithSuffix("group-", highest + 1);
}

// ------------------------------------------------------------ serialization --

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

void to_json(nlohmann::json& j, const EqInstanceConfig& e) {
    j = nlohmann::json{
        {"id", e.id},
        {"display_name", e.displayName},
        {"bypassed", e.bypassed},
        {"bands", e.bands},
    };
}

void to_json(nlohmann::json& j, const OutputChannelConfig& c) {
    j = nlohmann::json{
        {"position", c.position},
        {"device_position", c.devicePosition},
        {"display_name", c.displayName},
        {"gain_db", c.gainDb},
        {"muted", c.muted},
        {"eq_instance_id", c.eqInstanceId},
        {"sends_db", c.sendsDb},
    };
}

void to_json(nlohmann::json& j, const LinkGroupConfig& g) {
    j = nlohmann::json{
        {"id", g.id},
        {"display_name", g.displayName},
        {"channels", g.channelIndices},
    };
}

void to_json(nlohmann::json& j, const OutputConfig& o) {
    j = nlohmann::json{
        {"id", o.id},
        {"device_name", o.deviceName},
        {"display_name", o.displayName},
        {"auto_connect", o.autoConnect},
        {"eq_instances", o.eqInstances},
        {"channels", o.channels},
        {"link_groups", o.linkGroups},
    };
}

void to_json(nlohmann::json& j, const InputConfig& i) {
    j = nlohmann::json{
        {"id", i.id},
        {"display_name", i.displayName},
        {"positions", i.positions},
    };
}

void to_json(nlohmann::json& j, const AppConfig& c) {
    j = nlohmann::json{
        {"version", c.version},
        {"inputs", c.inputs},
        {"outputs", c.outputs},
    };
}

// ------------------------------------------------------------------ parsing --

namespace {

std::optional<EqBand> parseBand(const nlohmann::json& j) {
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

std::optional<EqInstanceConfig> parseEqInstance(const nlohmann::json& j,
                                                std::vector<std::string>& warnings) {
    if (!j.is_object()) {
        warnings.push_back("skipped an EQ instance that isn't a JSON object");
        return std::nullopt;
    }
    EqInstanceConfig instance;
    instance.id = readOr<std::string>(j, "id", "");
    if (instance.id.empty()) {
        warnings.push_back("skipped an EQ instance with no id");
        return std::nullopt;
    }
    instance.displayName = readOr<std::string>(j, "display_name", instance.id);
    instance.bypassed = readOr(j, "bypassed", false);

    const auto bands = j.find("bands");
    if (bands != j.end() && bands->is_array()) {
        for (const auto& bandJson : *bands) {
            if (auto band = parseBand(bandJson)) {
                instance.bands.push_back(*band);
            } else {
                warnings.push_back("skipped a malformed band in EQ instance '" + instance.id + "'");
            }
        }
    }
    return instance;
}

std::optional<OutputChannelConfig> parseChannel(const nlohmann::json& j,
                                                 std::vector<std::string>& warnings) {
    if (!j.is_object()) {
        warnings.push_back("skipped an output channel that isn't a JSON object");
        return std::nullopt;
    }
    OutputChannelConfig channel;
    channel.position = readOr<std::string>(j, "position", "");
    channel.devicePosition = readOr<std::string>(j, "device_position", "");
    channel.displayName = readOr<std::string>(j, "display_name", "");
    channel.gainDb = readOr(j, "gain_db", 0.0);
    channel.muted = readOr(j, "muted", false);
    channel.eqInstanceId = readOr<std::string>(j, "eq_instance_id", "");
    channel.sendsDb = readOr(j, "sends_db", std::map<std::string, double>{});
    return channel;
}

std::optional<LinkGroupConfig> parseLinkGroup(const nlohmann::json& j,
                                              std::vector<std::string>& warnings) {
    if (!j.is_object()) {
        warnings.push_back("skipped a link group that isn't a JSON object");
        return std::nullopt;
    }
    LinkGroupConfig group;
    group.id = readOr<std::string>(j, "id", "");
    if (group.id.empty()) {
        warnings.push_back("skipped a link group with no id");
        return std::nullopt;
    }
    group.displayName = readOr<std::string>(j, "display_name", "");
    group.channelIndices = readOr(j, "channels", std::vector<uint32_t>{});
    return group;
}

std::optional<OutputConfig> parseOutput(const nlohmann::json& j, std::vector<std::string>& warnings) {
    if (!j.is_object()) {
        warnings.push_back("skipped an output that isn't a JSON object");
        return std::nullopt;
    }
    OutputConfig output;
    output.id = readOr<std::string>(j, "id", "");
    output.deviceName = readOr<std::string>(j, "device_name", "");
    if (output.id.empty() || output.deviceName.empty()) {
        warnings.push_back("skipped an output with no id or no device_name");
        return std::nullopt;
    }
    output.displayName = readOr<std::string>(j, "display_name", output.deviceName);
    output.autoConnect = readOr(j, "auto_connect", true);

    if (const auto it = j.find("eq_instances"); it != j.end() && it->is_array()) {
        for (const auto& instanceJson : *it) {
            if (auto instance = parseEqInstance(instanceJson, warnings)) {
                output.eqInstances.push_back(std::move(*instance));
            }
        }
    }
    if (const auto it = j.find("channels"); it != j.end() && it->is_array()) {
        for (const auto& channelJson : *it) {
            if (auto channel = parseChannel(channelJson, warnings)) {
                output.channels.push_back(std::move(*channel));
            }
        }
    }
    if (const auto it = j.find("link_groups"); it != j.end() && it->is_array()) {
        for (const auto& groupJson : *it) {
            if (auto group = parseLinkGroup(groupJson, warnings)) {
                output.linkGroups.push_back(std::move(*group));
            }
        }
    }
    return output;
}

std::optional<InputConfig> parseInput(const nlohmann::json& j, std::vector<std::string>& warnings) {
    if (!j.is_object()) {
        warnings.push_back("skipped an input that isn't a JSON object");
        return std::nullopt;
    }
    InputConfig input;
    input.id = readOr<std::string>(j, "id", "");
    if (input.id.empty()) {
        warnings.push_back("skipped an input with no id");
        return std::nullopt;
    }
    input.displayName = readOr<std::string>(j, "display_name", input.id);
    input.positions = readOr(j, "positions", std::vector<std::string>{"FL", "FR"});
    if (input.positions.empty()) {
        input.positions = {"FL", "FR"};
        warnings.push_back("input '" + input.id + "' had an empty channel layout; assuming stereo");
    }
    return input;
}

} // namespace

std::optional<AppConfig> parseV2(const nlohmann::json& j, std::vector<std::string>& warnings) {
    if (!j.is_object()) {
        warnings.push_back("config root is not a JSON object");
        return std::nullopt;
    }

    const int version = readOr(j, "version", 1);
    if (version != kConfigVersion) {
        warnings.push_back("config version " + std::to_string(version) + " is not v" +
                           std::to_string(kConfigVersion));
        return std::nullopt;
    }

    AppConfig config;
    config.version = kConfigVersion;

    if (const auto it = j.find("inputs"); it != j.end()) {
        if (!it->is_array()) {
            warnings.push_back("\"inputs\" is not an array; ignoring it");
        } else {
            for (const auto& inputJson : *it) {
                if (auto input = parseInput(inputJson, warnings)) {
                    config.inputs.push_back(std::move(*input));
                }
            }
        }
    }

    const auto outputs = j.find("outputs");
    if (outputs == j.end()) {
        warnings.push_back("config has no \"outputs\" key");
    } else if (!outputs->is_array()) {
        warnings.push_back("\"outputs\" is not an array");
        return std::nullopt;
    } else {
        for (const auto& outputJson : *outputs) {
            if (auto output = parseOutput(outputJson, warnings)) {
                config.outputs.push_back(std::move(*output));
            }
        }
    }

    sanitize(config, warnings);
    return config;
}

// --------------------------------------------------------------- sanitizing --

void sanitize(AppConfig& config, std::vector<std::string>& warnings) {
    config.version = kConfigVersion;

    // Inputs: drop duplicates and remember which ids are real, so sends can be
    // pruned against them below.
    std::set<std::string> inputIds;
    for (auto it = config.inputs.begin(); it != config.inputs.end();) {
        if (!inputIds.insert(it->id).second) {
            warnings.push_back("dropped a duplicate input id '" + it->id + "'");
            it = config.inputs.erase(it);
            continue;
        }
        if (it->displayName.empty()) {
            it->displayName = it->id;
        }
        ++it;
    }

    std::set<std::string> outputIds;
    for (auto outputIt = config.outputs.begin(); outputIt != config.outputs.end();) {
        OutputConfig& output = *outputIt;

        if (!outputIds.insert(output.id).second) {
            warnings.push_back("dropped a duplicate output id '" + output.id + "'");
            outputIt = config.outputs.erase(outputIt);
            continue;
        }
        if (output.displayName.empty()) {
            output.displayName = output.deviceName;
        }

        // EQ instances: drop duplicates, clamp band counts.
        std::set<std::string> eqIds;
        for (auto it = output.eqInstances.begin(); it != output.eqInstances.end();) {
            if (!eqIds.insert(it->id).second) {
                warnings.push_back("output '" + output.id + "': dropped a duplicate EQ instance id '" +
                                   it->id + "'");
                it = output.eqInstances.erase(it);
                continue;
            }
            if (it->bands.size() > kMaxBands) {
                warnings.push_back("output '" + output.id + "': EQ instance '" + it->id + "' has " +
                                   std::to_string(it->bands.size()) + " bands; keeping the first " +
                                   std::to_string(kMaxBands));
                it->bands.resize(kMaxBands);
            }
            if (it->displayName.empty()) {
                it->displayName = it->id;
            }
            ++it;
        }

        // Channels: clear dangling EQ references, prune sends naming inputs
        // that no longer exist (the same pruning applyConfig() used to do).
        for (std::size_t i = 0; i < output.channels.size(); ++i) {
            OutputChannelConfig& channel = output.channels[i];
            if (!channel.eqInstanceId.empty() && eqIds.count(channel.eqInstanceId) == 0) {
                warnings.push_back("output '" + output.id + "' channel " + std::to_string(i) +
                                   ": EQ instance '" + channel.eqInstanceId +
                                   "' doesn't exist; leaving the channel with no EQ");
                channel.eqInstanceId.clear();
            }
            for (auto sendIt = channel.sendsDb.begin(); sendIt != channel.sendsDb.end();) {
                if (inputIds.count(sendIt->first) == 0) {
                    warnings.push_back("output '" + output.id + "' channel " + std::to_string(i) +
                                       ": dropped a send for unknown input '" + sendIt->first + "'");
                    sendIt = channel.sendsDb.erase(sendIt);
                } else {
                    ++sendIt;
                }
            }
        }

        // Link groups: drop duplicates and out-of-range members, reject
        // overlapping membership (a channel in two groups has no coherent
        // meaning - a set on it would have to write two different sets).
        std::set<std::string> groupIds;
        std::set<uint32_t> claimed;
        for (auto it = output.linkGroups.begin(); it != output.linkGroups.end();) {
            if (!groupIds.insert(it->id).second) {
                warnings.push_back("output '" + output.id + "': dropped a duplicate link group id '" +
                                   it->id + "'");
                it = output.linkGroups.erase(it);
                continue;
            }

            std::vector<uint32_t> members;
            for (uint32_t index : it->channelIndices) {
                if (index >= output.channels.size()) {
                    // Not a warning: a retired channel index is expected after
                    // a profile switch shrinks the device, and the group is
                    // meant to survive it. Kept out of `members` only so the
                    // live set stays valid.
                    members.push_back(index);
                    continue;
                }
                if (!claimed.insert(index).second) {
                    warnings.push_back("output '" + output.id + "': channel " + std::to_string(index) +
                                       " is in more than one link group; removed it from '" + it->id +
                                       "'");
                    continue;
                }
                members.push_back(index);
            }
            it->channelIndices = std::move(members);

            if (it->channelIndices.size() < 2) {
                warnings.push_back("output '" + output.id + "': dropped link group '" + it->id +
                                   "' with fewer than two members");
                it = output.linkGroups.erase(it);
                continue;
            }
            ++it;
        }

        ++outputIt;
    }
}

} // namespace eqcore
