cmake_minimum_required(VERSION 3.20)
project(pipeeq LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

include(GNUInstallDirs)

option(PIPEEQ_BUILD_GUI "Build the Qt6 control GUI" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)

find_package(nlohmann_json REQUIRED)
find_package(sdbus-c++ REQUIRED)
find_package(Threads REQUIRED)

add_subdirectory(common)
add_subdirectory(daemon)
add_subdirectory(packaging)

if(PIPEEQ_BUILD_GUI)
    find_package(Qt6 COMPONENTS Widgets)
    if(Qt6Widgets_FOUND)
        add_subdirectory(gui)
    else()
        message(WARNING "Qt6 Widgets not found - skipping pipeeq-gui target")
    endif()
endif()
