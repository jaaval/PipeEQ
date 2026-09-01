add_library(eqcore STATIC
    app_config.cpp
    biquad.cpp
    config_io.cpp
    config_migrate.cpp
    eq_response.cpp
)

target_include_directories(eqcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(eqcore PUBLIC nlohmann_json::nlohmann_json)
