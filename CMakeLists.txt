cmake_minimum_required(VERSION 3.20)
project(pipeeq LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

include(GNUInstallDirs)
include(CTest)

option(PIPEEQ_BUILD_GUI "Build the Qt6 control GUI" ON)
option(PIPEEQ_BUILD_TESTS "Build the unit test suites (ctest)" ON)

# e.g. -DPIPEEQ_SANITIZE=address,undefined or =thread. Off by default because
# the RT audio path must not carry sanitizer overhead in a normal build, but
# the ring buffer and snapshot publication are exactly the kind of code worth
# running the test suites under TSan/ASan.
set(PIPEEQ_SANITIZE "" CACHE STRING "Sanitizers to enable (comma separated), e.g. address,undefined")
if(PIPEEQ_SANITIZE)
    message(STATUS "Enabling sanitizers: ${PIPEEQ_SANITIZE}")
    add_compile_options(-fsanitize=${PIPEEQ_SANITIZE} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${PIPEEQ_SANITIZE})
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)

find_package(nlohmann_json REQUIRED)
find_package(sdbus-c++ REQUIRED)
find_package(Threads REQUIRED)

# sdbus-c++ 2.0 was a breaking API rewrite (strong ServiceName/ObjectPath/
# InterfaceName types, addVTable() replacing per-item registerMethod()+
# finishRegistration()). Distro-shipped versions vary a lot - e.g. Arch has
# 2.x, Ubuntu/Pop!_OS 24.04 ships 1.x - so both call shapes are compiled in,
# selected via this macro, rather than requiring a specific version.
if(NOT DEFINED sdbus-c++_VERSION_MAJOR)
    message(FATAL_ERROR "find_package(sdbus-c++) did not report a version; can't select the correct API shape")
endif()
message(STATUS "Building against sdbus-c++ ${sdbus-c++_VERSION} (major version ${sdbus-c++_VERSION_MAJOR})")
add_compile_definitions(SDBUSCPP_MAJOR_VERSION=${sdbus-c++_VERSION_MAJOR})

add_subdirectory(common)
add_subdirectory(daemon)
add_subdirectory(packaging)

if(PIPEEQ_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(PIPEEQ_BUILD_GUI)
    find_package(Qt6 COMPONENTS Widgets)
    if(Qt6Widgets_FOUND)
        add_subdirectory(gui)
    else()
        message(WARNING "Qt6 Widgets not found - skipping pipeeq-gui target")
    endif()
endif()
