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
# -DETHERNET_W5500="sck,miso,mosi,cs[,irq[,reset]]": a W5500 module on SPI,
# wired to these GPIOs (tesserkit::boards::w5500()).
if(DEFINED ETHERNET_W5500)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "ETHERNET_W5500=${ETHERNET_W5500}")
endif()
# -DBATTERY_GPIO=n: the Battery service on that ADC1 pin, through a divider of
# two equal resistors.
if(DEFINED BATTERY_GPIO)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BATTERY_GPIO=${BATTERY_GPIO})
endif()
