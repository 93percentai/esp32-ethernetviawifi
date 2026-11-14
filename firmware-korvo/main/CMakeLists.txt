set(common "${CMAKE_CURRENT_LIST_DIR}/../../firmware/main")
idf_component_register(
    SRCS "app_main.c" "bridge.c" "usb_ethernet.c" "ecm_protocol.c" "usb_net.c" "usb_net_class.c" "usb_net_native.c" "vendor/rtl8152_port.c" "diagnostics.c" "log_buffer.c" "display.c"
         "${common}/form_codec.c" "${common}/wifi_mgr.c" "${common}/config_store.c" "${common}/provisioning.c"
    INCLUDE_DIRS "." "${common}"
    REQUIRES nvs_flash esp_wifi esp_event esp_netif esp_timer esp_http_server
             esp32_s31_korvo_1 usb lvgl dns_server)
if(NOT IDF_TARGET STREQUAL "esp32s31")
    message(FATAL_ERROR "The Korvo build requires idf.py --preview set-target esp32s31")
endif()
