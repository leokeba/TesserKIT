cmake_minimum_required(VERSION 3.16)
# A test firmware for PulseOutput (docs/DESIGN.md section 5.21): pulse trains
# on one pin, driven over HTTP, and the pin's own edges timed by an MCPWM
# capture on the same pin. TesserAPI and TesserUI are found through
# TESSERAPI_PATH and TESSERUI_PATH, by default checkouts next to TesserKIT.
get_filename_component(TESSERKIT_PATH "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
foreach(dep TESSERAPI TESSERUI)
    if(DEFINED ENV{${dep}_PATH})
        get_filename_component(${dep}_PATH "$ENV{${dep}_PATH}" ABSOLUTE)
    elseif(dep STREQUAL "TESSERAPI")
        get_filename_component(${dep}_PATH "${TESSERKIT_PATH}/../TesserAPI" ABSOLUTE)
    else()
        get_filename_component(${dep}_PATH "${TESSERKIT_PATH}/../TesserUI" ABSOLUTE)
    endif()
endforeach()
set(EXTRA_COMPONENT_DIRS "${TESSERKIT_PATH}" "${TESSERAPI_PATH}" "${TESSERUI_PATH}")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tesserkit_pulses_probe)
