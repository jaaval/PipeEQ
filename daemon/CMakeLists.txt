add_executable(pipeeq-daemon
    main.cpp
    audio_engine.cpp
    output_route.cpp
    input_source.cpp
    dbus_service.cpp
)

target_link_libraries(pipeeq-daemon PRIVATE eqcore PkgConfig::PIPEWIRE Threads::Threads SDBusCpp::sdbus-c++)

install(TARGETS pipeeq-daemon RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
