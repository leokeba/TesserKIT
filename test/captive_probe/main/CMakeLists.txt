idf_component_register(SRCS "main.cpp" INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_http_client esp_timer lwip)
# idf.py -DAP_SSID=tesserkit-demo-a67c: the device's access point.
if(DEFINED AP_SSID)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE AP_SSID="${AP_SSID}")
endif()
