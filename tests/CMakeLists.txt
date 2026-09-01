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
        ${CMAKE_SOURCE_DIR}/gui/widgets/fader_taper.cpp
    )
    target_include_directories(test_gui_model PRIVATE ${CMAKE_SOURCE_DIR}/gui)
    target_link_libraries(test_gui_model PRIVATE pipeeq_test_support eqcore Qt6::Widgets)
    add_test(NAME test_gui_model COMMAND test_gui_model)
endif()
