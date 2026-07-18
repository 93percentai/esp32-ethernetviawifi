idf_component_register(
    SRCS
        "app_main.c"
        "bridge.c"
        "wifi_mgr.c"
        "config_store.c"
        "console.c"
        "display.c"
        "status_led.c"
        "provisioning.c"
        "button.c"
        "usb_gadget.c"
        "sdcard.c"
        "msc.c"
        "hid.c"
        "net_tether.c"
        "httpd_share.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_timer
        esp_http_server
        driver
        esp_driver_gpio
        esp_driver_spi
        esp_driver_sdmmc
        sdmmc
        fatfs
        esp_lcd
        esp_lcd_st7735
        espressif__esp_tinyusb
        dns_server
)

# Inject our tusb_config.h ahead of esp_tinyusb's so we can force the MSC class
# driver on (CFG_TUD_MSC=1) while providing our own tud_msc_* callbacks. The
# override uses #include_next to pull in esp_tinyusb's config; it must be applied
# to both the TinyUSB stack library and esp_tinyusb (which computes STRID_MAC
# from CFG_TUD_MSC and must agree with our custom NCM descriptor).
# Path buffers are intentionally bounded (FAT 8.3/LFN limits); the compiler's
# worst-case format-truncation analysis is overly conservative here.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=format-truncation)

set(_tusb_override "${CMAKE_CURRENT_SOURCE_DIR}/tusb_override")
target_include_directories(${COMPONENT_LIB} BEFORE PRIVATE "${_tusb_override}")
foreach(_lib espressif__tinyusb espressif__esp_tinyusb)
    idf_component_get_property(_lib_target ${_lib} COMPONENT_LIB)
    if(_lib_target)
        target_include_directories(${_lib_target} BEFORE PRIVATE "${_tusb_override}")
    endif()
endforeach()
