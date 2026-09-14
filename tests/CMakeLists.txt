add_executable(espressio_ota_foundation_test ota_foundation_test.cpp)
target_link_libraries(espressio_ota_foundation_test PRIVATE ESPressio::OTA)
target_compile_options(espressio_ota_foundation_test PRIVATE -Wall -Wextra -Werror -pedantic -fno-rtti -fno-exceptions)
add_test(NAME espressio_ota_foundation_test COMMAND espressio_ota_foundation_test)

add_executable(espressio_ota_manifest_test ota_manifest_test.cpp)
target_link_libraries(espressio_ota_manifest_test PRIVATE ESPressio::OTA)
target_compile_options(espressio_ota_manifest_test PRIVATE -Wall -Wextra -Werror -pedantic -fno-rtti -fno-exceptions)
add_test(NAME espressio_ota_manifest_test COMMAND espressio_ota_manifest_test)

add_executable(espressio_ota_offer_test ota_offer_test.cpp)
target_link_libraries(espressio_ota_offer_test PRIVATE ESPressio::OTA)
target_compile_options(espressio_ota_offer_test PRIVATE -Wall -Wextra -Werror -pedantic -fno-rtti -fno-exceptions)
add_test(NAME espressio_ota_offer_test COMMAND espressio_ota_offer_test)
