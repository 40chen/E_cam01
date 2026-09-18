idf_component_register(
    SRCS "lckfb_bsp.c"
    INCLUDE_DIRS "include"
    REQUIRES
        # IDF 5.1 里 LEDC / SPI / I2S / SDMMC 都还在 driver 组件内，
        # esp_driver_ledc 这类拆分出来的组件要到 5.3 才存在。
        driver
        esp_lcd
        esp_timer
        fatfs
        # 以下为托管组件，组件名即 managed_components/ 下的目录名
        espressif__esp32-camera
        espressif__esp_codec_dev
        espressif__esp_lcd_touch_ft5x06
        espressif__esp_lvgl_port
        lvgl__lvgl
)
