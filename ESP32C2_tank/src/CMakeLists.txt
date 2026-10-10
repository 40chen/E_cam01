# 这份是手写的，不是 PlatformIO 自动生成的那份。
#
# **为什么必须手写**：PlatformIO 那份只有
#     idf_component_register(SRCS ${app_sources})
# 没有 REQUIRES，于是这个组件只拿得到 IDF 的"公共依赖"（freertos/log/heap…），
# 而 nvs_flash / esp_wifi / esp_netif / esp_http_server 都不在里面——
# 编译能过，**链接期一片 undefined reference**。
#
# SRCS 显式列出来而不是 GLOB_RECURSE：加文件时多改一行，但不会出现
# "新写的 .c 没被编进去、却什么错都不报"。
idf_component_register(
    SRCS
        "main.c"
        "motion.c"
        "link.c"
        "udp.c"
        "wifi_prov.c"
        "web.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        driver
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
)
