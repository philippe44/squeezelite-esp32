set(COMPONENT_ADD_INCLUDEDIRS . )

set(COMPONENT_SRCS "bt_app_core.c" )
set(REQUIRES esp_common)
set(REQUIRES_COMPONENTS freertos nvs_flash esp32 spi_flash newlib log console )

register_component()
