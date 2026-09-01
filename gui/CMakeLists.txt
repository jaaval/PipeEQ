set(CMAKE_AUTOMOC ON)

add_executable(pipeeq-gui
    main.cpp
    main_window.cpp
    eq_curve_widget.cpp
    backend.cpp
    dbus_client.cpp
    fake_backend.cpp
    theme/theme.cpp
)

target_include_directories(pipeeq-gui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pipeeq-gui PRIVATE eqcore Qt6::Widgets SDBusCpp::sdbus-c++)

install(TARGETS pipeeq-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
