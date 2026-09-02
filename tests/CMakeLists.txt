# One executable per suite, each registered as a CTest test. They use the
# hand-rolled harness in check.h rather than a framework, so `ctest` works with
# no extra dependency to install.

add_library(pipeeq_test_support INTERFACE)
target_include_directories(pipeeq_test_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

function(pipeeq_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE pipeeq_test_support ${ARGN})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# Pure DSP math: needs libeqcore only.
pipeeq_add_test(test_dsp eqcore)

# Channel-layout helpers: pure, but the SPA position values live in PipeWire's
# header-only SPA headers, so it links the daemon core.
pipeeq_add_test(test_channel_layout pipeeq_core)

# Config schema, migration and the load/save path. Driven partly by a real v1
# config captured in tests/data, so the migration is asserted against a
# document the shipped daemon actually wrote.
pipeeq_add_test(test_config eqcore)
target_compile_definitions(test_config PRIVATE PIPEEQ_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

# The position-based routing rules - the core of multichannel behaviour.
pipeeq_add_test(test_mix_plan pipeeq_core)

# Lock-free peak metering, including a two-thread race check worth running
# under -DPIPEEQ_SANITIZE=thread.
pipeeq_add_test(test_peak_meter pipeeq_core)

# The lock-free ring buffer that carries audio from an input to every output.
pipeeq_add_test(test_ring_buffer pipeeq_core)

# The realtime DSP core: routing, ramps, per-channel EQ, NaN recovery, metering.
pipeeq_add_test(test_output_processor pipeeq_core)

# Asserts the realtime path allocates nothing. Overrides global operator new, so
# it must stay its own binary.
pipeeq_add_test(test_rt_no_alloc pipeeq_core)

# Reshaping an output's channels to its device's real layout without losing
# settings across a profile switch.
pipeeq_add_test(test_adopt_layout pipeeq_core)

# The GUI store's pure logic: write coalescing, edit guarding, meter ballistics.
# Needs Qt (QObject/QTimer) and therefore AUTOMOC, so it is built only when the
# GUI is.
if(TARGET pipeeq-gui)
    set(CMAKE_AUTOMOC ON)
    add_executable(test_gui_model
        test_gui_model.cpp
        ${CMAKE_SOURCE_DIR}/gui/backend.cpp
        ${CMAKE_SOURCE_DIR}/gui/model/edit_guard.cpp
        ${CMAKE_SOURCE_DIR}/gui/model/level_meters.cpp
        ${CMAKE_SOURCE_DIR}/gui/model/write_coalescer.cpp
        ${CMAKE_SOURCE_DIR}/gui/fake_backend.cpp
        ${CMAKE_SOURCE_DIR}/gui/widgets/fader_taper.cpp
    )
    target_include_directories(test_gui_model PRIVATE ${CMAKE_SOURCE_DIR}/gui)
    target_link_libraries(test_gui_model PRIVATE pipeeq_test_support eqcore Qt6::Widgets)
    add_test(NAME test_gui_model COMMAND test_gui_model)

    # The EQ band knobs. Almost entirely taper arithmetic, which is invisible in
    # a screenshot: a wrong logarithmic curve looks perfect and makes the bottom
    # two octaves unreachable. Every gesture is a real QEvent sent to the
    # widget, so no display is needed - it runs on the offscreen platform.
    add_executable(test_knob
        test_knob.cpp
        ${CMAKE_SOURCE_DIR}/gui/widgets/knob.cpp
        ${CMAKE_SOURCE_DIR}/gui/theme/theme.cpp
    )
    target_include_directories(test_knob PRIVATE ${CMAKE_SOURCE_DIR}/gui)
    target_link_libraries(test_knob PRIVATE pipeeq_test_support eqcore Qt6::Widgets)
    add_test(NAME test_knob COMMAND test_knob)

    # Drives the store end-to-end against the demo backend, including the
    # cross-thread hand-off to the backend worker - the seam where a call can
    # silently do nothing while every component looks correct in isolation.
    add_executable(test_app_state
        test_app_state.cpp
        ${CMAKE_SOURCE_DIR}/gui/backend.cpp
        ${CMAKE_SOURCE_DIR}/gui/backend_worker.cpp
        ${CMAKE_SOURCE_DIR}/gui/dbus_client.cpp
        ${CMAKE_SOURCE_DIR}/gui/fake_backend.cpp
        ${CMAKE_SOURCE_DIR}/gui/model/app_state.cpp
        ${CMAKE_SOURCE_DIR}/gui/model/edit_guard.cpp
        ${CMAKE_SOURCE_DIR}/gui/model/level_meters.cpp
        ${CMAKE_SOURCE_DIR}/gui/model/write_coalescer.cpp
    )
    target_include_directories(test_app_state PRIVATE ${CMAKE_SOURCE_DIR}/gui)
    target_link_libraries(test_app_state
        PRIVATE pipeeq_test_support eqcore Qt6::Widgets SDBusCpp::sdbus-c++)
    add_test(NAME test_app_state COMMAND test_app_state)
endif()
