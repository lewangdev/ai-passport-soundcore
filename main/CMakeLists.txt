idf_component_register(
    SRCS "main.c"
         "sc_audio.c"
         "sc_ble.c"
         "sc_core.c"
         "sc_crypto.c"
         "sc_proto.c"
         "sc_ring.c"
         "sc_store.c"
         "sc_ui.c"
         "sc_wifi.c"
         "sc_xfer.c"
    INCLUDE_DIRS "."
    REQUIRES bootloader_support bsp bt esp_event esp_hw_support esp_https_server esp_netif esp_timer esp_wifi
             mbedtls nvs_flash
)
