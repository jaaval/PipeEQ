set(CMAKE_AUTOMOC ON)

add_executable(pipeeq-gui
    main.cpp
    main_window.cpp
    eq_curve_widget.cpp
    backend.cpp
    backend_worker.cpp
    dbus_client.cpp
    fake_backend.cpp
    model/app_state.cpp
    model/edit_guard.cpp
    model/level_meters.cpp
    model/write_coalescer.cpp
    theme/paint_helpers.cpp
    theme/theme.cpp
    widgets/channel_strip.cpp
    widgets/detail_panel.cpp
    widgets/eq_preview.cpp
    widgets/fader_taper.cpp
    widgets/send_strip.cpp
    widgets/strip_rack.cpp
)

target_include_directories(pipeeq-gui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pipeeq-gui PRIVATE eqcore Qt6::Widgets SDBusCpp::sdbus-c++)

install(TARGETS pipeeq-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
