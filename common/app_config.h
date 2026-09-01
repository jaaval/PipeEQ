#pragma once

#include <cstdint>
#include <map>
#include <optional>
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

// Schema version of the config document. Bumped only for changes that a
// previous version's parser would misread; additive optional fields are read
// leniently and don't need a bump.
//
// v1: one "route" per stereo pair of a device, with a single flat band list
//     shared by both channels.
// v2: one "output" per device with N channels, each channel carrying its own
//     gain/mute/sends and referencing one of the output's named EQ instances.
inline constexpr int kConfigVersion = 2;

// The most bands one EQ instance can hold. Lives here rather than on the
// daemon's processor so the config layer can clamp a hand-edited or
// future-version file without depending on any of the audio code.
inline constexpr std::size_t kMaxBands = 16;

// One named EQ instance owned by an output. Several channels may reference the
// same instance - the FL/FR case - which is exactly why the bands live here
// rather than on the channel: editing one curve then edits every channel using
// it, with no "copy to the other side" step and no way for the two to drift
// apart.
struct EqInstanceConfig {
    std::string id;            // output-local, e.g. "eq-1"
    std::string displayName;   // "Mains", "Sub HPF"
    bool bypassed = false;
    std::vector<EqBand> bands; // clamped to kMaxBands on load

    bool operator==(const EqInstanceConfig&) const = default;
};

// One mono hardware output channel: the mixer strip unit.
struct OutputChannelConfig {
    // The logical channel position the user assigned to this physical channel
    // index, as a SPA short name ("FL", "LFE", "AUX3"). Defaults to whatever
    // the device advertises for this index.
    //
    // This is control-plane metadata ONLY. The stream is always negotiated
    // with the device's own layout, so changing this never reconnects
    // anything - it only changes which input channels the mix planner matches
    // into this channel. Declaring a position the device has no port for
    // would break the 1:1 link instead.
    std::string position;
    std::string displayName; // optional user label, e.g. "Monitor L"
    double gainDb = 0.0;
    bool muted = false;
    std::string eqInstanceId; // "" = no EQ on this channel

    // Send level per input id, in dB. An input ABSENT from this map is not
    // routed to this channel at all, which is deliberately distinct from a
    // 0 dB send: absent means no taps and no ring-buffer read, 0 dB means
    // unity.
    std::map<std::string, double> sendsDb;

    bool operator==(const OutputChannelConfig&) const = default;
};

// Channels joined so they share fader, mute and send levels.
//
// The group holds no values of its own: the per-channel fields above stay the
// single source of truth, and a set on any member writes every member. That
// keeps exactly one value per control, instead of a group value plus
// per-channel values that can disagree. On creation the group adopts the
// gain/mute/sends of its lowest-index member, so linking is never ambiguous.
struct LinkGroupConfig {
    std::string id;                       // output-local, e.g. "group-1"
    std::string displayName;
    std::vector<uint32_t> channelIndices; // indices into OutputConfig::channels

    bool operator==(const LinkGroupConfig&) const = default;
};

// One output: a physical device, every channel it drives, the EQ instances
// those channels reference, and how they're grouped.
//
// This is the *desired* configuration and the daemon's single source of truth,
// whether or not the target device happens to be present. An output whose
// device is missing stays here in full, so unplugging can never erase its EQ
// and it can be reconnected the moment the device reappears.
struct OutputConfig {
    std::string id;          // stable output id, e.g. "output-1"
    std::string deviceName;  // PipeWire node.name of the target physical sink
    std::string displayName; // user-facing label, defaults to deviceName
    bool autoConnect = true;

    // One entry per physical channel of the target device, in DEVICE channel
    // order, so index == physical channel index == stream channel index.
    //
    // Invariant: channels.size() may EXCEED the device's current channel
    // count. Entries at index >= the live channel count are retired configs,
    // kept so that a profile flip (4.0 -> stereo -> 4.0) restores the rear
    // channels' gain/EQ/sends rather than resetting them. Only the first
    // live-channel-count entries are driven.
    std::vector<OutputChannelConfig> channels;
    std::vector<EqInstanceConfig> eqInstances;
    std::vector<LinkGroupConfig> linkGroups;

    // Convenience accessors used all over the control plane. All return null
    // / npos rather than throwing, because ids come off the bus.
    EqInstanceConfig* findEqInstance(const std::string& eqId);
    const EqInstanceConfig* findEqInstance(const std::string& eqId) const;
    LinkGroupConfig* findLinkGroup(const std::string& groupId);
    const LinkGroupConfig* findLinkGroup(const std::string& groupId) const;
    // The group containing this channel index, or null if it's ungrouped.
    const LinkGroupConfig* groupOfChannel(std::size_t channelIndex) const;
    // The channel indices that a set on `channelIndex` must write: its whole
    // group, or just itself when ungrouped. Never empty for a valid index.
    std::vector<std::size_t> linkedChannels(std::size_t channelIndex) const;
    // An unused "eq-N" / "group-N" id for this output.
    std::string nextEqInstanceId() const;
    std::string nextLinkGroupId() const;

    bool operator==(const OutputConfig& other) const {
        return id == other.id && deviceName == other.deviceName && displayName == other.displayName &&
               autoConnect == other.autoConnect && channels == other.channels &&
               eqInstances == other.eqInstances && linkGroups == other.linkGroups;
    }
};

// One audio input: a virtual sink apps can be assigned to.
struct InputConfig {
    std::string id;          // stable id, e.g. "input-1", assigned by the daemon
    std::string displayName; // user-facing label

    // Declared channel layout of the virtual sink, as SPA short names. v1
    // inputs were always stereo, so migration yields {"FL","FR"}.
    //
    // Fixed at creation: changing it means destroying and recreating the
    // stream and its ring buffer, which every live output holds a shared_ptr
    // to, so the control API deliberately offers no setter for it.
    std::vector<std::string> positions{"FL", "FR"};

    bool operator==(const InputConfig&) const = default;
};

struct AppConfig {
    int version = kConfigVersion;
    std::vector<InputConfig> inputs;
    std::vector<OutputConfig> outputs;

    bool operator==(const AppConfig&) const = default;
};

void to_json(nlohmann::json& j, const EqInstanceConfig& e);
void to_json(nlohmann::json& j, const OutputChannelConfig& c);
void to_json(nlohmann::json& j, const LinkGroupConfig& g);
void to_json(nlohmann::json& j, const OutputConfig& o);
void to_json(nlohmann::json& j, const InputConfig& i);
void to_json(nlohmann::json& j, const AppConfig& c);

// Parses a v2 document.
//
// Structure-strict but field-lenient, on purpose: a missing or malformed
// individual output/channel/EQ instance is skipped with a warning and the rest
// of the document is kept, because losing five good outputs to one bad one is
// the worse failure. Returns nullopt only when the document as a whole isn't
// usable (not an object, wrong version, unreadable "outputs").
//
// There is no from_json(AppConfig) counterpart deliberately: a throwing parser
// is what let a single bad field turn into "zero outputs", which the caller
// then wrote back over a perfectly good file.
std::optional<AppConfig> parseV2(const nlohmann::json& j, std::vector<std::string>& warnings);

// Clamps everything the rest of the daemon can't represent - band counts,
// duplicate ids, dangling EQ instance references, out-of-range or overlapping
// link group members, sends naming inputs that don't exist. Appends a warning
// for each repair. Applied after both parseV2() and migrateV1(), so a
// hand-edited file and a migrated one get identical treatment.
void sanitize(AppConfig& config, std::vector<std::string>& warnings);

} // namespace eqcore
