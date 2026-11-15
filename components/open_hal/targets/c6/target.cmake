set(HAL_TARGET_SRCS
    "targets/c6/hw_usb.c"
    "targets/c6/hal_pmp.c"
    "targets/c6/hal_clock.c"
    "targets/c6/hal_wdt.c"
    "targets/c6/hal_ws2812.c"
    "targets/c6/hal_thermal.c"
    "targets/c6/hal_power.c"
)

set(HAL_TARGET_REQUIRES
    soc
    esp_timer
    esp_rom
)

set(HAL_TARGET_PRIV_REQUIRES
    esp_driver_gpio
    esp_driver_tsens
    esp_hw_support
    hal
)
