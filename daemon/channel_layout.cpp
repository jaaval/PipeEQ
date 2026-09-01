#include "channel_layout.h"

#include <array>

#include <spa/param/audio/raw-types.h>
#include <spa/param/audio/raw.h>

namespace pipeeq::layout {

std::vector<std::string> parseChannelPositions(const char* value) {
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

uint32_t positionValue(const std::string& name) {
    if (name.empty()) {
        return SPA_AUDIO_CHANNEL_UNKNOWN;
    }
    return spa_type_audio_channel_from_short_name(name.c_str());
}

std::string positionName(uint32_t value) {
    char buf[16];
    return spa_type_audio_channel_make_short_name(value, buf, sizeof(buf), "UNK");
}

std::vector<std::string> defaultPositionsFor(int numChannels) {
    if (numChannels <= 0) {
        return {};
    }

    switch (numChannels) {
    case 1:
        return {"MONO"};
    case 2:
        return {"FL", "FR"};
    case 3:
        return {"FL", "FR", "FC"};
    case 4:
        return {"FL", "FR", "RL", "RR"};
    case 5:
        return {"FL", "FR", "FC", "RL", "RR"};
    case 6:
        return {"FL", "FR", "FC", "LFE", "RL", "RR"};
    case 8:
        return {"FL", "FR", "FC", "LFE", "RL", "RR", "SL", "SR"};
    case 12:
        return {"FL", "FR", "FC", "LFE", "RL", "RR", "SL", "SR", "TFL", "TFR", "TRL", "TRR"};
    default:
        break;
    }

    std::vector<std::string> aux;
    aux.reserve(static_cast<std::size_t>(numChannels));
    for (int i = 0; i < numChannels; ++i) {
        aux.push_back("AUX" + std::to_string(i));
    }
    return aux;
}

bool needsRenegotiation(const std::vector<std::string>& oldPositions,
                        const std::vector<std::string>& newPositions) {
    return oldPositions != newPositions;
}

bool isFrontPosition(const std::string& name) {
    static const std::array<const char*, 4> kFront = {"FL", "FR", "FC", "MONO"};
    for (const char* candidate : kFront) {
        if (name == candidate) {
            return true;
        }
    }
    return false;
}

} // namespace pipeeq::layout
