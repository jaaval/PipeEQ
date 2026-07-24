add_library(eqcore STATIC
    biquad.cpp
    eq_chain.cpp
    route_config.cpp
)

target_include_directories(eqcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(eqcore PUBLIC nlohmann_json::nlohmann_json)

add_executable(eqcore_selftest selftest.cpp)
target_link_libraries(eqcore_selftest PRIVATE eqcore)
