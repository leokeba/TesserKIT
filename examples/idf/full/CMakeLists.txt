cmake_minimum_required(VERSION 3.16)
# TesserKIT is this repository; TesserAPI, TesserUI and NowTP are found
# through TESSERAPI_PATH, TESSERUI_PATH and NOWTP_PATH, by default checkouts
# next to it.
get_filename_component(TESSERKIT_PATH "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
foreach(dep TESSERAPI TESSERUI NOWTP)
    if(DEFINED ENV{${dep}_PATH})
        get_filename_component(${dep}_PATH "$ENV{${dep}_PATH}" ABSOLUTE)
    elseif(dep STREQUAL "TESSERAPI")
        get_filename_component(${dep}_PATH "${TESSERKIT_PATH}/../TesserAPI" ABSOLUTE)
    elseif(dep STREQUAL "TESSERUI")
        get_filename_component(${dep}_PATH "${TESSERKIT_PATH}/../TesserUI" ABSOLUTE)
    else()
        get_filename_component(${dep}_PATH "${TESSERKIT_PATH}/../NowTP" ABSOLUTE)
    endif()
    if(NOT EXISTS "${${dep}_PATH}/CMakeLists.txt")
        message(FATAL_ERROR "${dep} not found at ${${dep}_PATH}: clone it there, or set ${dep}_PATH")
    endif()
endforeach()
include("${TESSERKIT_PATH}/cmake/deps.cmake")
include("${TESSERKIT_PATH}/cmake/example.cmake")
tesserkit_check_sibling(TesserAPI "${TESSERAPI_PATH}")
tesserkit_check_sibling(TesserUI "${TESSERUI_PATH}")
tesserkit_check_sibling(NowTP "${NOWTP_PATH}")
set(EXTRA_COMPONENT_DIRS "${TESSERKIT_PATH}" "${TESSERAPI_PATH}" "${TESSERUI_PATH}" "${NOWTP_PATH}")
# What the firmware runs is in Kconfig (main/Kconfig.projbuild), so a profile
# is a configuration file (DESIGN.md 11, Profiles): -DPROFILE=classic reads
# profiles/classic.defaults after sdkconfig.defaults; without it, every
# service ("full"). The board options below are written as a defaults file
# too (cmake/example.cmake): build each variant in its own build directory,
# with -DSDKCONFIG=<build>/sdkconfig.
set(TESSERKIT_FULL_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
set(TESSERKIT_FULL_ARGS "")
if(DEFINED PROFILE AND NOT PROFILE STREQUAL "full")
    if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/profiles/${PROFILE}.defaults")
        message(FATAL_ERROR "no profile ${PROFILE}: profiles/ has the ones there are")
    endif()
    list(APPEND TESSERKIT_FULL_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/profiles/${PROFILE}.defaults")
    list(APPEND TESSERKIT_FULL_ARGS "CONFIG_FULL_PROFILE=\"${PROFILE}\"")
endif()
# -DETHERNET_BOARD=gledopto: the board's Ethernet, a preset of
# tesserkit::boards (gledopto, wt32Eth01, olimexEsp32Poe, esp32EthernetKit).
if(DEFINED ETHERNET_BOARD)
    set(boards gledopto GLEDOPTO wt32Eth01 WT32_ETH01 olimexEsp32Poe OLIMEX_ESP32_POE
               esp32EthernetKit ESP32_ETHERNET_KIT)
    list(FIND boards "${ETHERNET_BOARD}" i)
    if(i LESS 0)
        message(FATAL_ERROR "no Ethernet board ${ETHERNET_BOARD}: gledopto, wt32Eth01, olimexEsp32Poe or esp32EthernetKit")
    endif()
    math(EXPR i "${i} + 1")
    list(GET boards ${i} symbol)
    list(APPEND TESSERKIT_FULL_ARGS "CONFIG_FULL_ETHERNET_${symbol}=y")
endif()
# -DETHERNET_W5500="sck,miso,mosi,cs[,irq[,reset]]": a W5500 module on SPI,
# wired to these GPIOs (tesserkit::boards::w5500()).
if(DEFINED ETHERNET_W5500)
    string(REPLACE "," ";" pins "${ETHERNET_W5500}")
    list(LENGTH pins n)
    if(n LESS 4 OR n GREATER 6)
        message(FATAL_ERROR "ETHERNET_W5500 is sck,miso,mosi,cs[,irq[,reset]]: ${ETHERNET_W5500}")
    endif()
    list(APPEND TESSERKIT_FULL_ARGS "CONFIG_FULL_ETHERNET_W5500=y")
    set(names SCK MISO MOSI CS IRQ RESET)
    foreach(k RANGE 5)
        list(GET names ${k} name)
        set(pin -1)
        if(k LESS n)
            list(GET pins ${k} pin)
            string(STRIP "${pin}" pin)
        endif()
        list(APPEND TESSERKIT_FULL_ARGS "CONFIG_FULL_W5500_${name}=${pin}")
    endforeach()
endif()
# -DLED_GPIO=n: the LED's pin (default 2).
if(DEFINED LED_GPIO)
    list(APPEND TESSERKIT_FULL_ARGS "CONFIG_FULL_LED_GPIO=${LED_GPIO}")
endif()
# -DBATTERY_GPIO=n: the Battery service on that ADC1 pin, through a divider of
# two equal resistors.
if(DEFINED BATTERY_GPIO)
    list(APPEND TESSERKIT_FULL_ARGS "CONFIG_FULL_BATTERY_GPIO=${BATTERY_GPIO}")
endif()
tesserkit_example_defaults(TESSERKIT_FULL_DEFAULTS ${TESSERKIT_FULL_ARGS})
# A target's own defaults (sdkconfig.defaults.esp32c3, ...) follow each file.
set(SDKCONFIG_DEFAULTS "${TESSERKIT_FULL_DEFAULTS}")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tesserkit_full)
