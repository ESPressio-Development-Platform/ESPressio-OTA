cmake_minimum_required(VERSION 3.16)
project(ESPressioOTA LANGUAGES CXX)

add_library(ESPressio-OTA INTERFACE)
target_include_directories(ESPressio-OTA INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_compile_features(ESPressio-OTA INTERFACE cxx_std_17)
add_library(ESPressio::OTA ALIAS ESPressio-OTA)

# Optional source-tree hints used by native contract tests and super-projects.
# Package managers may instead supply these libraries through their normal
# dependency mechanism; the OTA public headers only consume their semantic APIs.
foreach(dependency System Platform Persistence Security Serializable)
    set(variable "ESPRESSIO_${dependency}_SOURCE_DIR")
    if(DEFINED ${variable} AND EXISTS "${${variable}}/src")
        target_include_directories(ESPressio-OTA INTERFACE "${${variable}}/src")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/deps/ESPressio-${dependency}/src")
        target_include_directories(ESPressio-OTA INTERFACE
            "${CMAKE_CURRENT_SOURCE_DIR}/deps/ESPressio-${dependency}/src")
    endif()
endforeach()

option(ESPRESSIO_OTA_BUILD_TESTS "Build ESPressio-OTA native contract tests" ON)
if(ESPRESSIO_OTA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
