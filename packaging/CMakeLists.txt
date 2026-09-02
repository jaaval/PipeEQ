# Both units embed an absolute path to the daemon, which has to be the path it
# is actually installed to. CMAKE_INSTALL_FULL_BINDIR resolves at configure
# time, so a `cmake --install --prefix` that differs from the configured prefix
# will still produce units pointing at the configured one - configure with the
# prefix you intend to install to.
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
# RELATIVE destination. Using ${CMAKE_INSTALL_PREFIX} baked the configure-time
# prefix into the path, so `cmake --install --prefix /opt/...` moved the binary
# but left both unit files under the old prefix, pointing ExecStart at a
# executable that no longer existed there.
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/pipeeq-daemon.service
        DESTINATION lib/systemd/user)

# Lets the GUI (or any D-Bus client) auto-start the daemon on first use via
# bus activation, independent of whether the systemd unit above is enabled.
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.pipeeq.Daemon1.service
        DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/services)
