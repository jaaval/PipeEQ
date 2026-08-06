#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <spa/param/audio/raw-types.h>
#include <spa/param/audio/raw.h>

namespace pipeeq {

// One selectable stereo pair on a device: which two channel positions it
// occupies, a label for the UI, and where it sits in the device's channel
// order. `leftName`/`rightName` empty means "whatever the device does by
// default" - the fallback for devices that don't advertise a layout PipeEQ
// recognizes, which connects the way it always has.
struct ChannelPair {
    std::string leftName;  // SPA short name, e.g. "FL"
    std::string rightName; // SPA short name, e.g. "FR"
    std::string label;     // e.g. "Rear L/R (ch 3-4)"
    bool isDefault() const { return leftName.empty() || rightName.empty(); }
};

// The stereo pairs PipeEQ knows how to drive, in the order they're offered.
// PipeEQ is stereo-only: an output is always exactly one of these pairs, so
// a 4.0 device is two independent outputs rather than one 4-channel one.
struct StereoPairDef {
    const char* left;
    const char* right;
    const char* label;
};

inline const std::vector<StereoPairDef>& knownStereoPairs() {
    static const std::vector<StereoPairDef> pairs = {
        {"FL", "FR", "Front L/R"},
        {"RL", "RR", "Rear L/R"},
        {"SL", "SR", "Side L/R"},
        {"FLC", "FRC", "Front Center L/R"},
        {"TFL", "TFR", "Top Front L/R"},
        {"TRL", "TRR", "Top Rear L/R"},
    };
    return pairs;
}

// Parses an "audio.position" property value - a comma separated list of SPA
// channel short names, optionally wrapped in brackets, e.g.
// "[ FL, FR, RL, RR ]" as PipeWire reports it for a 4.0 sink.
inline std::vector<std::string> parseChannelPositions(const char* value) {
    std::vector<std::string> names;
    if (!value) {
        return names;
    }

    std::string current;
    for (const char* c = value; *c; ++c) {
        if (*c == ',' || *c == '[' || *c == ']' || *c == ' ' || *c == '\t' || *c == '"') {
            if (!current.empty()) {
                names.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(*c);
    }
    if (!current.empty()) {
        names.push_back(current);
    }
    return names;
}

// The stereo pairs a device with these channel positions can offer. A device
// whose layout contains no recognized pair (no position info at all, a mono
// sink, aux-only channels) gets a single default pair, so it keeps working
// exactly as before this existed.
inline std::vector<ChannelPair> stereoPairsFor(const std::vector<std::string>& positions) {
    std::vector<ChannelPair> pairs;

    for (const auto& def : knownStereoPairs()) {
        std::size_t leftIndex = positions.size();
        std::size_t rightIndex = positions.size();
        for (std::size_t i = 0; i < positions.size(); ++i) {
            if (positions[i] == def.left) {
                leftIndex = i;
            } else if (positions[i] == def.right) {
                rightIndex = i;
            }
        }
        if (leftIndex == positions.size() || rightIndex == positions.size()) {
            continue;
        }

        ChannelPair pair;
        pair.leftName = def.left;
        pair.rightName = def.right;
        // 1-based channel numbers, which is how interfaces label their
        // physical outputs ("outputs 3/4" on a Scarlett 4i4).
        pair.label = std::string(def.label) + " (ch " + std::to_string(leftIndex + 1) + "-" +
                     std::to_string(rightIndex + 1) + ")";
        pairs.push_back(std::move(pair));
    }

    if (pairs.empty()) {
        pairs.push_back(ChannelPair{"", "", "Device default"});
    }
    return pairs;
}

// Resolves a SPA channel short name to its position value. Returns
// SPA_AUDIO_CHANNEL_UNKNOWN for an empty or unrecognized name.
inline uint32_t channelPositionFromName(const std::string& name) {
    if (name.empty()) {
        return SPA_AUDIO_CHANNEL_UNKNOWN;
    }
    return spa_type_audio_channel_from_short_name(name.c_str());
}

} // namespace pipeeq
