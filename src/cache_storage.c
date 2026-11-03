/* cache_storage.c -- Unified removable SD / internal Flash Cache VFS seam. */

#include "cache_storage.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "app_config.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"
#include "sdcard.h"
#include "wear_levelling.h"

static const char *TAG = "cache_storage";
static cache_backend_t s_backend = CACHE_BACKEND_NONE;
static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;

void cache_storage_quiesce(void)
{
    sdcard_quiesce();
}

bool cache_storage_mount(void)
{
    if (s_backend == CACHE_BACKEND_SDCARD && sdcard_mounted()) return true;
    if (s_backend == CACHE_BACKEND_FLASH && s_wl_handle != WL_INVALID_HANDLE) return true;

    /* 1. Prefer removable microSD card when inserted and readable. */
    if (sdcard_mounted() || sdcard_mount()) {
        s_backend = CACHE_BACKEND_SDCARD;
        return true;
    }

#if defined(TESSERAE_FLASH_CACHE) && TESSERAE_FLASH_CACHE
    /* 2. Fall back to internal wear-levelled Flash Cache partition. */
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "flash_cache");
    if (!part) {
        ESP_LOGW(TAG, "flash_cache partition not found in partition table");
        s_backend = CACHE_BACKEND_NONE;
        return false;
    }

    esp_vfs_fat_mount_config_t mnt = {
        .format_if_mount_failed = true,
        .max_files = 8,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
        .disk_status_check_enable = false,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(
        CACHE_FLASH_MOUNT_POINT, "flash_cache", &mnt, &s_wl_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "flash_cache mount failed: %s", esp_err_to_name(err));
        s_wl_handle = WL_INVALID_HANDLE;
        s_backend = CACHE_BACKEND_NONE;
        return false;
    }

    s_backend = CACHE_BACKEND_FLASH;
    ESP_LOGI(TAG, "mounted internal flash_cache at %s (%llu KB free)",
             CACHE_FLASH_MOUNT_POINT,
             (unsigned long long)(cache_storage_free_bytes() >> 10));
    return true;
#else
    s_backend = CACHE_BACKEND_NONE;
    return false;
#endif
}

bool cache_storage_mounted(void)
{
    if (sdcard_mounted()) return true;
    if (s_backend == CACHE_BACKEND_FLASH) return s_wl_handle != WL_INVALID_HANDLE;
    return false;
}

cache_backend_t cache_storage_backend(void)
{
    if (sdcard_mounted()) return CACHE_BACKEND_SDCARD;
    if (s_backend == CACHE_BACKEND_FLASH && s_wl_handle != WL_INVALID_HANDLE)
        return CACHE_BACKEND_FLASH;
    return CACHE_BACKEND_NONE;
}

const char *cache_storage_root(void)
{
    switch (cache_storage_backend()) {
    case CACHE_BACKEND_SDCARD: return SDCARD_MOUNT_POINT;
    case CACHE_BACKEND_FLASH:  return CACHE_FLASH_MOUNT_POINT;
    default:                   return NULL;
    }
}

uint64_t cache_storage_free_bytes(void)
{
    cache_backend_t b = cache_storage_backend();
    if (b == CACHE_BACKEND_SDCARD) return sdcard_free_bytes();
    if (b == CACHE_BACKEND_FLASH) {
        uint64_t total = 0, free_b = 0;
        if (esp_vfs_fat_info(CACHE_FLASH_MOUNT_POINT, &total, &free_b) != ESP_OK)
            return 0;
        return free_b;
    }
    return 0;
}

void cache_storage_unmount(void)
{
    if (s_backend == CACHE_BACKEND_FLASH && s_wl_handle != WL_INVALID_HANDLE) {
        esp_vfs_fat_spiflash_unmount_rw_wl(CACHE_FLASH_MOUNT_POINT, s_wl_handle);
        s_wl_handle = WL_INVALID_HANDLE;
    }
    if (s_backend == CACHE_BACKEND_SDCARD || sdcard_mounted()) {
        sdcard_unmount();
    }
    s_backend = CACHE_BACKEND_NONE;
}

void cache_storage_prepare_sleep(void)
{
    if (s_backend == CACHE_BACKEND_FLASH && s_wl_handle != WL_INVALID_HANDLE) {
        esp_vfs_fat_spiflash_unmount_rw_wl(CACHE_FLASH_MOUNT_POINT, s_wl_handle);
        s_wl_handle = WL_INVALID_HANDLE;
    }
    s_backend = CACHE_BACKEND_NONE;
    /* Always run sdcard_prepare_sleep() so slot/panel GPIO deep-sleep latches hold. */
    sdcard_prepare_sleep();
}

#else /* !ESP_PLATFORM -- Host unit test backend */

static char s_test_root[384];
static uint64_t s_test_free_bytes;
static cache_backend_t s_test_backend = CACHE_BACKEND_NONE;

void cache_storage_set_test_root(const char *root_dir,
                                 uint64_t sim_free_bytes,
                                 cache_backend_t backend)
{
    if (!root_dir || !root_dir[0] || backend == CACHE_BACKEND_NONE) {
        s_test_root[0] = '\0';
        s_test_free_bytes = 0;
        s_test_backend = CACHE_BACKEND_NONE;
        return;
    }
    strncpy(s_test_root, root_dir, sizeof(s_test_root) - 1);
    s_test_root[sizeof(s_test_root) - 1] = '\0';
    s_test_free_bytes = sim_free_bytes;
    s_test_backend = backend;
}

void cache_storage_quiesce(void) {}
bool cache_storage_mount(void) { return s_test_backend != CACHE_BACKEND_NONE; }
bool cache_storage_mounted(void) { return s_test_backend != CACHE_BACKEND_NONE && s_test_root[0] != '\0'; }
cache_backend_t cache_storage_backend(void) { return cache_storage_mounted() ? s_test_backend : CACHE_BACKEND_NONE; }
const char *cache_storage_root(void) { return cache_storage_mounted() ? s_test_root : NULL; }
uint64_t cache_storage_free_bytes(void) { return cache_storage_mounted() ? s_test_free_bytes : 0; }
void cache_storage_unmount(void) { s_test_root[0] = '\0'; s_test_free_bytes = 0; s_test_backend = CACHE_BACKEND_NONE; }
void cache_storage_prepare_sleep(void) { cache_storage_unmount(); }

#endif /* ESP_PLATFORM */

uint64_t cache_storage_effective_capacity_bytes(uint32_t frame_bytes)
{
    uint64_t free_b = cache_storage_free_bytes();
    if (free_b == 0 || frame_bytes == 0) return free_b;
    /* Reserve conservative 1:1 reporting when fewer than 2 raw frames fit so a
     * nearly-full partition never overcommits on incompressible images. */
    if (free_b < (uint64_t)frame_bytes * 2ULL) return free_b;
    return free_b * CACHE_COMPRESSION_FACTOR;
}
