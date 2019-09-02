idf_component_register(SRCS "bt_app_core.c" "bt_app_sink.c" "bt_app_source.c"
                       INCLUDE_DIRS . ../tools/
                       REQUIRES esp_common
                       PRIV_REQUIRES freertos bt io nvs_flash esp32 spi_flash newlib log console pthread
)

