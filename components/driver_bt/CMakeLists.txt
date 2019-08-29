set(COMPONENT_ADD_INCLUDEDIRS . )

set(COMPONENT_SRCS "bt_app_core.c" "bt_app_sink.c" "bt_app_source.c")
set(REQUIRES esp_common)
set(REQUIRES_COMPONENTS freertos nvs_flash esp32 spi_flash newlib log console )

register_component()
