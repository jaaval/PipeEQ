set(CMAKE_AUTOMOC ON)

add_executable(pipeeq-gui
    main.cpp
    main_window.cpp
    eq_curve_widget.cpp
    dbus_client.cpp
)

target_link_libraries(pipeeq-gui PRIVATE eqcore Qt6::Widgets SDBusCpp::sdbus-c++)

install(TARGETS pipeeq-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
