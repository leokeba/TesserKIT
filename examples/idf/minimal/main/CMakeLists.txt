idf_component_register(SRCS "main.cpp" INCLUDE_DIRS ".")
# Footprint (docs/DESIGN.md section 11): idf.py -DFOOTPRINT_BASELINE=1 builds
# the same application on TesserAPI and TesserUI alone.
if(DEFINED FOOTPRINT_BASELINE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FOOTPRINT_BASELINE=${FOOTPRINT_BASELINE})
endif()
# Rollback test (test/e2e_device.py --crash-image): -DCRASH_AT_BOOT=1 builds
# an image that panics before it can confirm itself.
if(DEFINED CRASH_AT_BOOT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CRASH_AT_BOOT=${CRASH_AT_BOOT})
endif()
# Provisioning test (test/captive_probe): -DNO_DEFAULT_NETWORK=1 ignores
# secrets.h's network, so a fresh device opens its access point.
if(DEFINED NO_DEFAULT_NETWORK)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NO_DEFAULT_NETWORK=${NO_DEFAULT_NETWORK})
endif()
# -DLED_GPIO=n: the LED's pin (default 2).
if(DEFINED LED_GPIO)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LED_GPIO=${LED_GPIO})
endif()
