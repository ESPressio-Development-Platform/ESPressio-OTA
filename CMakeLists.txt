cmake_minimum_required(VERSION 3.16)
project(ESPressioOTA LANGUAGES CXX)

add_library(ESPressio-OTA INTERFACE)
target_include_directories(ESPressio-OTA INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_compile_features(ESPressio-OTA INTERFACE cxx_std_17)
add_library(ESPressio::OTA ALIAS ESPressio-OTA)

option(ESPRESSIO_OTA_BUILD_TESTS "Build ESPressio-OTA native contract tests" ON)
if(ESPRESSIO_OTA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
