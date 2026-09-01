#include "adopt_layout.h"

#include <algorithm>
#include <cstdint>

namespace pipeeq {

AdoptResult adoptDeviceLayout(eqcore::OutputConfig& output,
                               const std::vector<std::string>& devicePositions) {
    AdoptResult result;
    result.liveChannelCount = devicePositions.size();

    const std::vector<eqcore::OutputChannelConfig> previous = output.channels;
    const std::size_t deviceChannels = devicePositions.size();

    std::vector<bool> consumed(previous.size(), false);
    std::vector<eqcore::OutputChannelConfig> live(deviceChannels);
    std::vector<bool> filled(deviceChannels, false);
    // newIndexOfOld[j] is where previous[j] ended up, or npos if unused so far.
    std::vector<std::size_t> newIndexOfOld(previous.size(), std::string::npos);

    // 1. Match by position name.
    for (std::size_t i = 0; i < deviceChannels; ++i) {
        for (std::size_t j = 0; j < previous.size(); ++j) {
            if (!consumed[j] && previous[j].position == devicePositions[i]) {
                live[i] = previous[j];
                consumed[j] = true;
                filled[i] = true;
                newIndexOfOld[j] = i;
                break;
            }
        }
    }

    // 2. Match anything left over by index, in order.
    std::size_t nextUnused = 0;
    for (std::size_t i = 0; i < deviceChannels; ++i) {
        if (filled[i]) {
            continue;
        }
        while (nextUnused < previous.size() && consumed[nextUnused]) {
            ++nextUnused;
        }
        if (nextUnused >= previous.size()) {
            break;
        }
        live[i] = previous[nextUnused];
        live[i].position = devicePositions[i]; // adopt the device's name for it
        consumed[nextUnused] = true;
        filled[i] = true;
        newIndexOfOld[nextUnused] = i;
    }

    // 3. Brand-new channels: present, editable, and silent.
    for (std::size_t i = 0; i < deviceChannels; ++i) {
        if (filled[i]) {
            continue;
        }
        eqcore::OutputChannelConfig fresh;
        fresh.position = devicePositions[i];
        live[i] = fresh;
        ++result.appendedChannels;
    }

    // Retired configs keep their settings after the live ones.
    std::vector<eqcore::OutputChannelConfig> retired;
    for (std::size_t j = 0; j < previous.size(); ++j) {
        if (!consumed[j]) {
            newIndexOfOld[j] = deviceChannels + retired.size();
            retired.push_back(previous[j]);
        }
    }
    result.retiredChannels = retired.size();

    std::vector<eqcore::OutputChannelConfig> combined = std::move(live);
    combined.insert(combined.end(), retired.begin(), retired.end());

    result.changed = combined.size() != previous.size();
    if (!result.changed) {
        for (std::size_t i = 0; i < combined.size(); ++i) {
            if (!(combined[i] == previous[i])) {
                result.changed = true;
                break;
            }
        }
    }

    output.channels = std::move(combined);

    // Remap link group membership through the move, so a reshuffle can't turn a
    // linked pair into two strips that only look linked.
    for (auto& group : output.linkGroups) {
        std::vector<uint32_t> remapped;
        remapped.reserve(group.channelIndices.size());
        for (uint32_t index : group.channelIndices) {
            if (index < newIndexOfOld.size() && newIndexOfOld[index] != std::string::npos) {
                remapped.push_back(static_cast<uint32_t>(newIndexOfOld[index]));
            } else if (index >= previous.size()) {
                // Already referred to something beyond the old list; leave it,
                // since it may become valid again on a later profile flip.
                remapped.push_back(index);
            }
        }
        std::sort(remapped.begin(), remapped.end());
        remapped.erase(std::unique(remapped.begin(), remapped.end()), remapped.end());
        group.channelIndices = std::move(remapped);
    }

    return result;
}

} // namespace pipeeq
