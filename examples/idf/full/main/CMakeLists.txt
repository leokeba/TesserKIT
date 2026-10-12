idf_component_register(SRCS "main.cpp" INCLUDE_DIRS ".")
# -DLED_GPIO=n: the LED's pin (default 2).
if(DEFINED LED_GPIO)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LED_GPIO=${LED_GPIO})
endif()
# -DETHERNET_BOARD=gledopto: the board's Ethernet, a preset of
# tesserkit::boards (gledopto, wt32Eth01, olimexEsp32Poe, esp32EthernetKit).
if(DEFINED ETHERNET_BOARD)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ETHERNET_BOARD=${ETHERNET_BOARD})
endif()
