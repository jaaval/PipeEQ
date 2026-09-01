# Split into a static library plus a thin executable so the daemon's own code -
# the mix planner, the channel-layout helpers, the RT processor - is linkable
# from the test binaries in tests/. main.cpp is the only thing left in the
# executable.
add_library(pipeeq_core STATIC
    adopt_layout.cpp
    audio_engine.cpp
    channel_layout.cpp
    mix_plan.cpp
    output_processor.cpp
    output_stream.cpp
    input_source.cpp
    dbus_service.cpp
    ring_buffer.cpp
)

target_include_directories(pipeeq_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pipeeq_core PUBLIC eqcore PkgConfig::PIPEWIRE Threads::Threads SDBusCpp::sdbus-c++)

add_executable(pipeeq-daemon main.cpp)
target_link_libraries(pipeeq-daemon PRIVATE pipeeq_core)

install(TARGETS pipeeq-daemon RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
