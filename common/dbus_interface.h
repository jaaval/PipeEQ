#pragma once

// D-Bus names shared between the daemon (adaptor) and the GUI (proxy) so they
// can't drift apart. Session bus only - this is a per-user service.

namespace eqcore::dbus {

inline constexpr const char* kServiceName = "org.pipeeq.Daemon1";
inline constexpr const char* kObjectPath = "/org/pipeeq/Daemon1";
inline constexpr const char* kInterfaceName = "org.pipeeq.Daemon1";

// Methods
inline constexpr const char* kMethodListDevices = "ListDevices";
inline constexpr const char* kMethodListRoutes = "ListRoutes";
inline constexpr const char* kMethodAddRoute = "AddRoute";
inline constexpr const char* kMethodRemoveRoute = "RemoveRoute";
inline constexpr const char* kMethodSetRouteGain = "SetRouteGain";
inline constexpr const char* kMethodSetRouteMute = "SetRouteMute";
inline constexpr const char* kMethodSetRouteBand = "SetRouteBand";
inline constexpr const char* kMethodSetRouteBandCount = "SetRouteBandCount";
inline constexpr const char* kMethodGetState = "GetState";
inline constexpr const char* kMethodGetRouteBands = "GetRouteBands";
inline constexpr const char* kMethodListInputs = "ListInputs";
inline constexpr const char* kMethodAddInput = "AddInput";
inline constexpr const char* kMethodRemoveInput = "RemoveInput";
inline constexpr const char* kMethodSetRouteInputGain = "SetRouteInputGain";
inline constexpr const char* kMethodRemoveRouteInput = "RemoveRouteInput";
inline constexpr const char* kMethodGetRouteInputGains = "GetRouteInputGains";
inline constexpr const char* kMethodGetMixMatrix = "GetMixMatrix";

// Signals
inline constexpr const char* kSignalDevicesChanged = "DevicesChanged";
inline constexpr const char* kSignalRouteChanged = "RouteChanged";
inline constexpr const char* kSignalInputsChanged = "InputsChanged";

} // namespace eqcore::dbus
