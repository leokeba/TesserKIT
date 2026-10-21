idf_component_register(SRCS "main.cpp" INCLUDE_DIRS ".")
# -DPULSES_GPIO=n: the pin (default 4, free on a Gledopto and on devkits).
if(DEFINED PULSES_GPIO)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PULSES_GPIO=${PULSES_GPIO})
endif()
