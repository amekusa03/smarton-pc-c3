idf_component_register(
    SRCS "app_main.cpp" "pc_control.c" "pc_monitor.c"
    PRIV_INCLUDE_DIRS "."
    REQUIRES esp_matter nvs_flash esp_driver_gpio freertos esp_netif mdns lwip
)

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
