# ESP-IDF component when built by idf.py, host library + tests otherwise.
set(TESSERKIT_CORE_SRCS
    src/tesserkit/device.cpp
    src/tesserkit/image.cpp
    src/tesserkit/system.cpp
    src/tesserkit/network.cpp
    src/tesserkit/time_service.cpp
    src/tesserkit/firmware.cpp
    src/tesserkit/backup.cpp
    src/tesserkit/files.cpp
    src/tesserkit/stored_backups.cpp
    src/tesserkit/crypto.cpp
    src/tesserkit/auth.cpp
    src/tesserkit/timers.cpp
    src/tesserkit/sequencer.cpp
    src/tesserkit/dmx.cpp
    src/tesserkit/artnet.cpp
    src/tesserkit/sacn.cpp
    src/tesserkit/mqtt.cpp
    src/tesserkit/bluetooth.cpp
    src/tesserkit/nowtp.cpp
    src/tesserkit/log.cpp
    src/tesserkit/battery.cpp
    src/tesserkit/sleep.cpp
    src/tesserkit/udp_api.cpp
    src/tesserkit/peers.cpp
    src/tesserkit/pwm.cpp
    src/tesserkit/pulses.cpp
    src/tesserkit/board.cpp
    src/tesserkit/button.cpp
    src/tesserkit/settings_store.cpp
    src/tesserkit/limits.cpp)

if(ESP_PLATFORM)
    # TesserAPI and TesserUI are hard dependencies. Their component names
    # depend on how they were added: the repository's directory
    # (EXTRA_COMPONENT_DIRS), or "<namespace>__<name>" (component manager).
    # Requirements are resolved before the component list is known, so they
    # are found here and linked below rather than listed in REQUIRES.
    idf_build_get_property(TESSERKIT_BUILD_COMPONENTS BUILD_COMPONENTS)
    set(TESSERKIT_TESSERAPI "")
    set(TESSERKIT_TESSERUI "")
    foreach(c ${TESSERKIT_BUILD_COMPONENTS})
        string(TOLOWER "${c}" c_lower)
        if(c_lower MATCHES "(^|__)tesserapi$")
            set(TESSERKIT_TESSERAPI ${c})
        elseif(c_lower MATCHES "(^|__)tesserui$")
            set(TESSERKIT_TESSERUI ${c})
        endif()
    endforeach()
    if((NOT TESSERKIT_TESSERAPI OR NOT TESSERKIT_TESSERUI) AND NOT CMAKE_BUILD_EARLY_EXPANSION)
        message(FATAL_ERROR "TesserKIT needs TesserAPI (https://github.com/leokeba/TesserAPI) and TesserUI "
                            "(https://github.com/leokeba/TesserUI): add them to EXTRA_COMPONENT_DIRS or as "
                            "component manager dependencies.")
    endif()
    # The temperature sensor, GPIO, SPI, LEDC and GPTimer drivers left "driver" in ESP-IDF 5.3.
    if(IDF_VERSION_MAJOR GREATER 5 OR (IDF_VERSION_MAJOR EQUAL 5 AND IDF_VERSION_MINOR GREATER_EQUAL 3))
        set(TESSERKIT_DRIVER_REQS esp_driver_tsens esp_driver_gpio esp_driver_spi esp_driver_ledc esp_driver_gptimer)
    else()
        set(TESSERKIT_DRIVER_REQS driver)
    endif()
    idf_component_register(
        SRCS ${TESSERKIT_CORE_SRCS}
             src/tesserkit/platform/esp/device_esp.cpp
             src/tesserkit/platform/esp/core_esp.cpp
             src/tesserkit/platform/esp/wifi_esp.cpp
             src/tesserkit/platform/esp/eth_esp.cpp
             src/tesserkit/platform/esp/ip_stack_esp.cpp
             src/tesserkit/platform/esp/mdns_esp.cpp
             src/tesserkit/platform/esp/dns_server.cpp
             src/tesserkit/platform/esp/clock_esp.cpp
             src/tesserkit/platform/esp/ota_esp.cpp
             src/tesserkit/platform/esp/http_client_esp.cpp
             src/tesserkit/platform/esp/coredump_esp.cpp
             src/tesserkit/platform/esp/udp_esp.cpp
             src/tesserkit/platform/esp/udp_groups_esp.cpp
             src/tesserkit/platform/esp/mqtt_esp.cpp
             src/tesserkit/platform/esp/ble_esp.cpp
             src/tesserkit/platform/esp/nowtp_esp.cpp
             src/tesserkit/platform/esp/filesystem_esp.cpp
             src/tesserkit/platform/esp/log_esp.cpp
             src/tesserkit/platform/esp/power_esp.cpp
             src/tesserkit/platform/esp/pins_esp.cpp
             src/tesserkit/platform/esp/adc_esp.cpp
             src/tesserkit/platform/esp/pwm_esp.cpp
             src/tesserkit/platform/esp/pulses_esp.cpp
             src/tesserkit/platform/esp/udp_api_esp.cpp
        INCLUDE_DIRS "src"
        REQUIRES esp_http_server esp_wifi esp_netif nvs_flash
        PRIV_REQUIRES esp_event esp_timer app_update esp_http_client mbedtls lwip mdns mqtt espcoredump joltwallet__littlefs
                      esp_partition spi_flash efuse esp_app_format esp_eth esp_adc ${TESSERKIT_DRIVER_REQS})
    if(NOT CONFIG_TESSERKIT_MDNS)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE TESSERKIT_NO_MDNS=1)
    endif()
    if(NOT CONFIG_TESSERKIT_HOME_ASSISTANT)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE TESSERKIT_NO_HOME_ASSISTANT=1)
    endif()
    # Public: TesserKIT's headers include TesserAPI's and TesserUI's.
    if(TESSERKIT_TESSERAPI AND TESSERKIT_TESSERUI)
        idf_component_get_property(TESSERKIT_TESSERAPI_LIB ${TESSERKIT_TESSERAPI} COMPONENT_LIB)
        idf_component_get_property(TESSERKIT_TESSERUI_LIB ${TESSERKIT_TESSERUI} COMPONENT_LIB)
        target_link_libraries(${COMPONENT_LIB} PUBLIC ${TESSERKIT_TESSERAPI_LIB} ${TESSERKIT_TESSERUI_LIB})
    endif()
    return()
endif()

cmake_minimum_required(VERSION 3.16)
project(tesserkit CXX)

option(TESSERKIT_BUILD_TESTS "Build host-side tests" ON)
option(TESSERKIT_SANITIZE "Build tests with ASan/UBSan" ON)
set(TESSERAPI_DIR "" CACHE PATH "TesserAPI source tree (default: ../TesserAPI if present, else fetched from GitHub)")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# TesserAPI, for tesser_core (and ArduinoJson through it). The host build
# needs nothing from TesserUI: WebUi is ESP-only.
# A project that already has TesserAPI (through TesserMotion, say) keeps its own.
if(NOT TARGET tesser_core)
    set(TESSER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    if(NOT TESSERAPI_DIR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../TesserAPI/CMakeLists.txt")
        get_filename_component(TESSERAPI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../TesserAPI" ABSOLUTE)
        set(TESSERAPI_DIR "${TESSERAPI_DIR}" CACHE PATH "" FORCE)
    endif()
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/deps.cmake")
    if(TESSERAPI_DIR)
        message(STATUS "TesserAPI: ${TESSERAPI_DIR}")
        tesserkit_check_sibling(TesserAPI "${TESSERAPI_DIR}")
        add_subdirectory("${TESSERAPI_DIR}" "${CMAKE_BINARY_DIR}/tesserapi")
    else()
        # The commit deps.lock pins, as CI's releases use.
        tesserkit_pinned(TesserAPI TESSERAPI_SHA)
        message(STATUS "TesserAPI: fetching https://github.com/leokeba/TesserAPI.git at ${TESSERAPI_SHA}")
        include(FetchContent)
        FetchContent_Declare(tesserapi
            GIT_REPOSITORY https://github.com/leokeba/TesserAPI.git
            GIT_TAG ${TESSERAPI_SHA})
        FetchContent_MakeAvailable(tesserapi)
    endif()
endif()

set(TESSERKIT_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror -fno-exceptions -fno-rtti)

add_library(tesserkit_core ${TESSERKIT_CORE_SRCS})
target_include_directories(tesserkit_core PUBLIC src)
target_link_libraries(tesserkit_core PUBLIC tesser_core)
set_target_properties(tesserkit_core PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(tesserkit_core PRIVATE ${TESSERKIT_WARNINGS})

# The fake platform (test/fake_platform.h), also for applications' host tests
# and simulators: link tesserkit_fake and include "fake_platform.h".
add_library(tesserkit_fake test/fake_platform.cpp)
target_include_directories(tesserkit_fake PUBLIC test)
target_link_libraries(tesserkit_fake PUBLIC tesserkit_core)
set_target_properties(tesserkit_fake PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(tesserkit_fake PRIVATE ${TESSERKIT_WARNINGS})

if(TESSERKIT_BUILD_TESTS)
    enable_testing()
    add_executable(tesserkit_tests test/test_main.cpp test/alloc_cap.cpp)
    add_executable(tesserkit_fixtures test/fixtures_main.cpp)
    # A device on the host for working on its interface (not run by ctest).
    add_executable(tesserkit_demo test/demo_main.cpp)
    foreach(t tesserkit_tests tesserkit_fixtures tesserkit_demo)
        target_link_libraries(${t} PRIVATE tesserkit_fake)
        set_target_properties(${t} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
        target_compile_options(${t} PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
    endforeach()
    if(TESSERKIT_SANITIZE)
        # tesser_core too, so the whole binary is instrumented alike.
        foreach(t tesser_core tesserkit_core tesserkit_fake tesserkit_tests tesserkit_fixtures tesserkit_demo)
            target_compile_options(${t} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -g)
            target_link_options(${t} PRIVATE -fsanitize=address,undefined)
        endforeach()
    endif()
    # The allocator cap names where an allocation came from (dladdr).
    if(NOT APPLE)
        target_link_options(tesserkit_tests PRIVATE -rdynamic)
    endif()
    add_test(NAME tesserkit_tests COMMAND tesserkit_tests)
    # Checks that the generator runs cleanly; the committed fixtures are
    # regenerated with `./build/tesserkit_fixtures fixtures`.
    add_test(NAME tesserkit_fixtures COMMAND tesserkit_fixtures ${CMAKE_CURRENT_BINARY_DIR}/fixtures)
endif()
