/*
 * cache_storage.h: Unified storage seam for Deck, Collection, Overlay & Spec caches.
 *
 * Mount order on device:
 *   1. Removable microSD card at "/sdcard" when present and readable.
 *   2. Internal wear-levelled Flash Cache partition ("flash_cache") at "/flash"
 *      on builds defining TESSERAE_FLASH_CACHE.
 *
 * Host tests drive the exact same public seam via cache_storage_set_test_root().
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CACHE_BACKEND_NONE = 0,
    CACHE_BACKEND_SDCARD = 1,
    CACHE_BACKEND_FLASH = 2,
} cache_backend_t;

#define CACHE_FLASH_MOUNT_POINT "/flash"
#define CACHE_COMPRESSION_FACTOR 4u

void cache_storage_quiesce(void);
bool cache_storage_mount(void);
bool cache_storage_mounted(void);
cache_backend_t cache_storage_backend(void);

/* Active VFS mount prefix (e.g. "/sdcard" or "/flash"), or NULL when unmounted. */
const char *cache_storage_root(void);

/* Actual raw free bytes on the active VFS, or 0 when unmounted. */
uint64_t cache_storage_free_bytes(void);

/* Effective uncompressed frame capacity advertised to the companion server.
 * Scales raw free space by CACHE_COMPRESSION_FACTOR once free space exceeds
 * two uncompressed frames, clamping to raw free bytes when nearly full so a
 * tight partition never overcommits. */
uint64_t cache_storage_effective_capacity_bytes(uint32_t frame_bytes);

void cache_storage_unmount(void);
void cache_storage_prepare_sleep(void);

#ifndef ESP_PLATFORM
void cache_storage_set_test_root(const char *root_dir,
                                 uint64_t sim_free_bytes,
                                 cache_backend_t backend);
#endif
