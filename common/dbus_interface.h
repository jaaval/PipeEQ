#pragma once

// D-Bus names shared between the daemon (adaptor) and the GUI (proxy) so they
// can't drift apart. Session bus only - this is a per-user service.
//
// The v1 surface was "route"-shaped: one method per stereo-pair output. It is
// gone rather than aliased, because an alias would have had to silently pick a
// channel, and a caller that thought it was setting an output's gain would have
// been setting one channel's. A clean break is easier to reason about, and the
// daemon and GUI ship together.

namespace eqcore::dbus {

inline constexpr const char* kServiceName = "org.pipeeq.Daemon1";
inline constexpr const char* kObjectPath = "/org/pipeeq/Daemon1";
inline constexpr const char* kInterfaceName = "org.pipeeq.Daemon1";

// ---------------------------------------------------------------- readers --
inline constexpr const char* kMethodListDevices = "ListDevices";
inline constexpr const char* kMethodListOutputs = "ListOutputs";
inline constexpr const char* kMethodGetOutputChannels = "GetOutputChannels";
inline constexpr const char* kMethodListEqInstances = "ListEqInstances";
inline constexpr const char* kMethodGetEqBands = "GetEqBands";
inline constexpr const char* kMethodGetChannelEqBands = "GetChannelEqBands";
inline constexpr const char* kMethodListLinkGroups = "ListLinkGroups";
inline constexpr const char* kMethodGetSends = "GetSends";
inline constexpr const char* kMethodListInputs = "ListInputs";
inline constexpr const char* kMethodGetState = "GetState";

// ---------------------------------------------------------------- outputs --
inline constexpr const char* kMethodAddOutput = "AddOutput";
inline constexpr const char* kMethodRemoveOutput = "RemoveOutput";
inline constexpr const char* kMethodSetOutputDisplayName = "SetOutputDisplayName";
inline constexpr const char* kMethodSetOutputAutoConnect = "SetOutputAutoConnect";

// --------------------------------------------------------------- channels --
inline constexpr const char* kMethodSetChannelGain = "SetChannelGain";
inline constexpr const char* kMethodSetChannelMuted = "SetChannelMuted";
inline constexpr const char* kMethodSetChannelPosition = "SetChannelPosition";
inline constexpr const char* kMethodSetChannelDisplayName = "SetChannelDisplayName";
inline constexpr const char* kMethodSetChannelEqInstance = "SetChannelEqInstance";

// --------------------------------------------------------------------- EQ --
inline constexpr const char* kMethodAddEqInstance = "AddEqInstance";
inline constexpr const char* kMethodRemoveEqInstance = "RemoveEqInstance";
inline constexpr const char* kMethodSetEqInstanceName = "SetEqInstanceName";
inline constexpr const char* kMethodSetEqBypassed = "SetEqBypassed";
inline constexpr const char* kMethodSetEqBandCount = "SetEqBandCount";
inline constexpr const char* kMethodSetEqBand = "SetEqBand";
inline constexpr const char* kMethodCopyEqInstance = "CopyEqInstance";
// Channel-scoped convenience: resolves a channel to its EQ instance, creating
// one on demand. Lets a caller edit a channel's curve without knowing that
// instances exist at all.
inline constexpr const char* kMethodSetChannelEqBandCount = "SetChannelEqBandCount";
inline constexpr const char* kMethodSetChannelEqBand = "SetChannelEqBand";

// ------------------------------------------------------------ link groups --
inline constexpr const char* kMethodCreateLinkGroup = "CreateLinkGroup";
inline constexpr const char* kMethodRemoveLinkGroup = "RemoveLinkGroup";
inline constexpr const char* kMethodSetLinkGroupChannels = "SetLinkGroupChannels";

// ------------------------------------------------------------------ sends --
inline constexpr const char* kMethodSetSend = "SetSend";
inline constexpr const char* kMethodRemoveSend = "RemoveSend";

// ----------------------------------------------------------------- inputs --
inline constexpr const char* kMethodAddInput = "AddInput";
inline constexpr const char* kMethodRemoveInput = "RemoveInput";
inline constexpr const char* kMethodSetInputDisplayName = "SetInputDisplayName";

// --------------------------------------------------------------- metering --
// Arms the Meters signal for a few seconds; callers re-arm while they're
// watching. A lease rather than a subscription so a crashed client stops
// metering on its own, with no bus-name tracking on the daemon side.
inline constexpr const char* kMethodSetMeteringEnabled = "SetMeteringEnabled";

// ---------------------------------------------------------------- signals --
inline constexpr const char* kSignalDevicesChanged = "DevicesChanged";
inline constexpr const char* kSignalOutputsChanged = "OutputsChanged";
// Carries the output id. Deliberately per-OUTPUT rather than per-channel: a set
// on a linked channel mutates every member of its group, so a per-channel
// signal would leave the partner's fader stale in the UI.
inline constexpr const char* kSignalOutputChanged = "OutputChanged";
inline constexpr const char* kSignalInputsChanged = "InputsChanged";
inline constexpr const char* kSignalMeters = "Meters";

} // namespace eqcore::dbus
