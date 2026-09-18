#include "app_storage.h"

#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "lckfb_bsp.h"

static const char *TAG = "storage";
static bool s_ready = false;

static void ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        return;
    }
    if (mkdir(path, 0777) != 0) {
        ESP_LOGW(TAG, "mkdir %s failed (errno %d)", path, errno);
    }
}

esp_err_t app_storage_init(void)
{
    esp_err_t err = bsp_sdcard_mount();
    if (err != ESP_OK) {
        s_ready = false;
        /* 常见原因：卡槽空着、卡是 exFAT（本工程只支持 FAT32）、接触不良。
         * 这里只告警不中断，界面会明确提示 TF 卡不可用。 */
        ESP_LOGW(TAG, "TF card unavailable: %s", esp_err_to_name(err));
        return err;
    }

    ensure_dir(APP_PHOTO_DIR);
    ensure_dir(APP_MUSIC_DIR);
    s_ready = true;
    ESP_LOGI(TAG, "storage ready, free %llu KB", app_storage_free_bytes() / 1024);
    return ESP_OK;
}

esp_err_t app_storage_deinit(void)
{
    esp_err_t err = bsp_sdcard_unmount();
    s_ready = false;
    return err;
}

esp_err_t app_storage_remount(void)
{
    bsp_sdcard_unmount();
    return app_storage_init();
}

bool app_storage_ready(void)
{
    return s_ready;
}

uint64_t app_storage_free_bytes(void)
{
    if (!s_ready) {
        return 0;
    }
    /* IDF 的 newlib 没有 statvfs，用 FATFS 自己的接口 */
    uint64_t total = 0, free_bytes = 0;
    if (esp_vfs_fat_info(BSP_SD_MOUNT_POINT, &total, &free_bytes) != ESP_OK) {
        return 0;
    }
    return free_bytes;
}

esp_err_t app_storage_list(const char *dir, const char *suffix,
                           app_storage_iter_cb_t cb, void *user)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    DIR *d = opendir(dir);
    if (d == NULL) {
        ESP_LOGW(TAG, "opendir %s failed", dir);
        return ESP_FAIL;
    }

    size_t suffix_len = suffix ? strlen(suffix) : 0;
    struct dirent *ent;
    esp_err_t ret = ESP_OK;

    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        if (suffix_len > 0) {
            size_t n = strlen(ent->d_name);
            if (n < suffix_len || strcasecmp(ent->d_name + n - suffix_len, suffix) != 0) {
                continue;
            }
        }
        if (!cb(ent->d_name, user)) {
            break;
        }
    }

    closedir(d);
    return ret;
}
