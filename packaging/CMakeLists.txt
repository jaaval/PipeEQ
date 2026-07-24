# Both units point at wherever `cmake --install` puts pipeeq-daemon, so the
# generated files always match this build's actual install location.
set(PIPEEQ_DAEMON_PATH "${CMAKE_INSTALL_FULL_BINDIR}/pipeeq-daemon")

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/systemd/pipeeq-daemon.service.in
    ${CMAKE_CURRENT_BINARY_DIR}/pipeeq-daemon.service
    @ONLY
)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/dbus/org.pipeeq.Daemon1.service.in
    ${CMAKE_CURRENT_BINARY_DIR}/org.pipeeq.Daemon1.service
    @ONLY
)

# This is a per-user service (PipeWire itself normally runs per-user too),
# so it belongs under the systemd *user* unit directory, not the system one.
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/pipeeq-daemon.service
        DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/user)

# Lets the GUI (or any D-Bus client) auto-start the daemon on first use via
# bus activation, independent of whether the systemd unit above is enabled.
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.pipeeq.Daemon1.service
        DESTINATION ${CMAKE_INSTALL_PREFIX}/share/dbus-1/services)
