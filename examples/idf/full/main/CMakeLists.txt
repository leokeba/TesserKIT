idf_component_register(SRCS "main.cpp" INCLUDE_DIRS ".")
# -DLED_GPIO=n: the LED's pin (default 2).
if(DEFINED LED_GPIO)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LED_GPIO=${LED_GPIO})
endif()
