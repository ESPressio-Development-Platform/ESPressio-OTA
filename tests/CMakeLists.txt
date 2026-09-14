add_executable(espressio_ota_foundation_test ota_foundation_test.cpp)
target_link_libraries(espressio_ota_foundation_test PRIVATE ESPressio::OTA)
target_compile_options(espressio_ota_foundation_test PRIVATE -Wall -Wextra -Werror -pedantic -fno-rtti -fno-exceptions)
add_test(NAME espressio_ota_foundation_test COMMAND espressio_ota_foundation_test)

add_executable(espressio_ota_composition_test ota_composition_test.cpp)
target_link_libraries(espressio_ota_composition_test PRIVATE ESPressio::OTA)
target_compile_options(espressio_ota_composition_test PRIVATE -Wall -Wextra -Werror -pedantic -fno-rtti -fno-exceptions)
add_test(NAME espressio_ota_composition_test COMMAND espressio_ota_composition_test)

add_executable(espressio_ota_manifest_test ota_manifest_test.cpp)
target_link_libraries(espressio_ota_manifest_test PRIVATE ESPressio::OTA)
target_compile_options(espressio_ota_manifest_test PRIVATE -Wall -Wextra -Werror -pedantic -fno-rtti -fno-exceptions)
add_test(NAME espressio_ota_manifest_test COMMAND espressio_ota_manifest_test)

add_executable(espressio_ota_offer_test ota_offer_test.cpp)
target_link_libraries(espressio_ota_offer_test PRIVATE ESPressio::OTA)
target_compile_options(espressio_ota_offer_test PRIVATE -Wall -Wextra -Werror -pedantic -fno-rtti -fno-exceptions)
add_test(NAME espressio_ota_offer_test COMMAND espressio_ota_offer_test)

add_executable(espressio_ota_providers_test ota_providers_test.cpp)
target_link_libraries(espressio_ota_providers_test PRIVATE ESPressio::OTA)
target_compile_options(espressio_ota_providers_test PRIVATE -Wall -Wextra -Werror -pedantic -fno-rtti -fno-exceptions)
add_test(NAME espressio_ota_providers_test COMMAND espressio_ota_providers_test)

add_executable(espressio_ota_catalog_test ota_catalog_test.cpp)
target_link_libraries(espressio_ota_catalog_test PRIVATE ESPressio::OTA)
target_compile_options(espressio_ota_catalog_test PRIVATE -Wall -Wextra -Werror -pedantic -fno-rtti -fno-exceptions)
add_test(NAME espressio_ota_catalog_test COMMAND espressio_ota_catalog_test)

add_executable(espressio_ota_component_handler_test ota_component_handler_test.cpp)
target_link_libraries(espressio_ota_component_handler_test PRIVATE ESPressio::OTA)
target_compile_options(espressio_ota_component_handler_test PRIVATE -Wall -Wextra -Werror -pedantic -fno-rtti -fno-exceptions)
add_test(NAME espressio_ota_component_handler_test COMMAND espressio_ota_component_handler_test)

add_executable(espressio_ota_update_plan_test ota_update_plan_test.cpp)
target_link_libraries(espressio_ota_update_plan_test PRIVATE ESPressio::OTA)
target_compile_options(espressio_ota_update_plan_test PRIVATE -Wall -Wextra -Werror -pedantic -fno-rtti -fno-exceptions)
add_test(NAME espressio_ota_update_plan_test COMMAND espressio_ota_update_plan_test)

add_executable(espressio_ota_durable_test ota_durable_test.cpp)
target_link_libraries(espressio_ota_durable_test PRIVATE ESPressio::OTA)
target_compile_options(espressio_ota_durable_test PRIVATE -Wall -Wextra -Werror -pedantic -fno-rtti -fno-exceptions)
add_test(NAME espressio_ota_durable_test COMMAND espressio_ota_durable_test)

add_executable(espressio_ota_state_test ota_state_test.cpp)
target_link_libraries(espressio_ota_state_test PRIVATE ESPressio::OTA)
target_compile_options(espressio_ota_state_test PRIVATE -Wall -Wextra -Werror -pedantic -fno-rtti -fno-exceptions)
add_test(NAME espressio_ota_state_test COMMAND espressio_ota_state_test)
