#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pipeeq::layout {

// Channel-layout helpers, kept pure and free of PipeWire objects so they can be
// unit tested without a graph, a daemon or a session bus. SPA headers are
// header-only, so a test only needs to link PkgConfig::PIPEWIRE to use these.
//
// Positions are carried as SPA short-name strings ("FL", "LFE", "AUX3")
// throughout the control plane and the config file, and converted to SPA
// position values only where a stream format is actually built. That is what
// keeps libeqcore free of any SPA dependency while still letting the config
// name channel positions.

// Parses an "audio.position" property value - a comma separated list of SPA
// channel short names, optionally wrapped in brackets, e.g. "[ FL, FR, RL, RR ]"
// as PipeWire reports it for a 4.0 sink.
std::vector<std::string> parseChannelPositions(const char* value);

// Resolves a SPA channel short name to its position value. Returns
// SPA_AUDIO_CHANNEL_UNKNOWN for an empty or unrecognized name. "AUX<n>" works.
uint32_t positionValue(const std::string& name);

// The inverse. Returns "UNK" for a value with no name, and "AUX<n>" for the aux
// range, so a round trip through positionValue() is stable for both.
std::string positionName(uint32_t value);

// The conventional layout for a channel count, used for a device or a virtual
// sink that doesn't advertise one. Known counts get their standard layout
// (2 -> FL/FR, 6 -> 5.1, 8 -> 7.1, 12 -> 7.1.4); anything else gets
// AUX0..AUX{n-1}, which is honest about "we don't know what these are" rather
// than guessing a layout the hardware doesn't have.
std::vector<std::string> defaultPositionsFor(int numChannels);

// True when moving from one layout to the other requires renegotiating a
// stream's format, i.e. the channel count or any position changed. A pure
// property refresh that reports the same layout must return false, or every
// device property update would tear down and rebuild the stream.
bool needsRenegotiation(const std::vector<std::string>& oldPositions,
                        const std::vector<std::string>& newPositions);

// True for the positions a single-channel (mono) source should feed at unity.
// Front-ish positions only: a mono input belongs in the mains and the center,
// not silently in the LFE or the rears.
bool isFrontPosition(const std::string& name);

} // namespace pipeeq::layout
