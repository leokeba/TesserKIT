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
tesserkit_check_sibling(TesserAPI "${TESSERAPI_PATH}")
tesserkit_check_sibling(TesserUI "${TESSERUI_PATH}")
tesserkit_check_sibling(NowTP "${NOWTP_PATH}")
set(EXTRA_COMPONENT_DIRS "${TESSERKIT_PATH}" "${TESSERAPI_PATH}" "${TESSERUI_PATH}" "${NOWTP_PATH}")
# -DPROFILE=classic: profiles/classic.defaults read after sdkconfig.defaults
# (DESIGN.md 11, Profiles). Without it, every service ("full"). The settings
# apply to a new sdkconfig: build each profile in its own build directory,
# with -DSDKCONFIG=<build>/sdkconfig.
if(DEFINED PROFILE AND NOT PROFILE STREQUAL "full")
    if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/profiles/${PROFILE}.defaults")
        message(FATAL_ERROR "no profile ${PROFILE}: profiles/ has the ones there are")
    endif()
    set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/profiles/${PROFILE}.defaults")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tesserkit_full)
