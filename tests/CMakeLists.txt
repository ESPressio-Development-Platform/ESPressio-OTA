add_executable(espressio_ota_foundation_test ota_foundation_test.cpp)
target_link_libraries(espressio_ota_foundation_test PRIVATE ESPressio::OTA)
target_compile_options(espressio_ota_foundation_test PRIVATE -Wall -Wextra -Werror -pedantic -fno-rtti -fno-exceptions)
add_test(NAME espressio_ota_foundation_test COMMAND espressio_ota_foundation_test)
