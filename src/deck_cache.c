/* deck_cache.c -- Unified VFS file layer for the compressed deck frame cache. */

#ifndef ESP_PLATFORM
#define _POSIX_C_SOURCE 200809L
#endif

#include "deck_cache.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cache_storage.h"
#include "frame_codec.h"

#ifdef ESP_PLATFORM
#include "app_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#else
static inline void deck_log_noop(const char *tag, const char *fmt, ...)
{
    (void)tag;
    (void)fmt;
}
#define ESP_LOGW deck_log_noop
#define ESP_LOGI deck_log_noop
#endif

static const char *TAG = "deck_cache";

/* Manifests are small JSON; cap well above DECK_MAX_PAGES worth of content. */
#define DECK_MANIFEST_MAX (16 * 1024)
#define SD_BOUNCE_BYTES   (32 * 1024)

static void *fb_alloc(size_t n)
{
#ifdef ESP_PLATFORM
    void *p = heap_caps_malloc(n, TESSERAE_FB_CAPS);
    if (p) return p;
#endif
    return malloc(n);
}

static uint8_t *bounce_alloc(size_t *cap)
{
#ifdef ESP_PLATFORM
    for (size_t n = SD_BOUNCE_BYTES; n >= 512; n /= 2) {
        uint8_t *p = heap_caps_malloc(n, MALLOC_CAP_DMA);
        if (p) { *cap = n; return p; }
    }
    ESP_LOGW(TAG, "no DMA-capable bounce buffer (largest free %u)",
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    return NULL;
#else
    uint8_t *p = malloc(SD_BOUNCE_BYTES);
    if (p) *cap = SD_BOUNCE_BYTES;
    return p;
#endif
}

static bool bounce_write_fd(int fd, const uint8_t *src, size_t len)
{
    size_t cap = 0;
    uint8_t *chunk = bounce_alloc(&cap);
    if (!chunk) return false;
    bool ok = true;
    for (size_t off = 0; ok && off < len; off += cap) {
        size_t n = len - off > cap ? cap : len - off;
        memcpy(chunk, src + off, n);
        ssize_t w = write(fd, chunk, n);
        ok = w == (ssize_t)n;
        if (!ok) ESP_LOGW(TAG, "write returned %d of %u (errno %d)", (int)w, (unsigned)n, errno);
    }
    free(chunk);
    return ok;
}

static bool bounce_read_fd(int fd, uint8_t *dst, size_t cap, size_t *out_len)
{
    if (out_len) *out_len = 0;
    size_t bcap = 0;
    uint8_t *chunk = bounce_alloc(&bcap);
    if (!chunk) return false;
    size_t total = 0;
    bool ok = true;
    while (total < cap) {
        size_t want = cap - total > bcap ? bcap : cap - total;
        ssize_t n = read(fd, chunk, want);
        if (n > 0) {
            memcpy(dst + total, chunk, (size_t)n);
            total += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) ok = false;
        break;
    }
    free(chunk);
    if (out_len) *out_len = total;
    return ok;
}

/* deck_id becomes a directory name: restrict it to filesystem-safe chars so a
 * hostile server value cannot traverse paths. */
static bool deck_id_safe(const char *id)
{
    if (!id || !id[0] || strlen(id) >= DECK_ID_CAP) return false;
    for (const char *p = id; *p; p++) {
        bool ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9') || *p == '-' || *p == '_';
        if (!ok) return false;
    }
    return true;
}

static bool deck_dir(const char *deck_id, char *out, size_t cap)
{
    const char *root = cache_storage_root();
    if (!root || !deck_id_safe(deck_id)) return false;
    int n = snprintf(out, cap, "%s/tesserae/decks/%s", root, deck_id);
    return n > 0 && n < (int)cap;
}

/* mkdir -p for the three fixed levels; EEXIST is success. */
static bool ensure_deck_dir(const char *deck_id)
{
    const char *root = cache_storage_root();
    char l1[256], l2[256], path[256];
    if (!root || !deck_dir(deck_id, path, sizeof path)) return false;
    snprintf(l1, sizeof l1, "%s/tesserae", root);
    snprintf(l2, sizeof l2, "%s/tesserae/decks", root);
    const char *levels[] = { l1, l2, path };
    for (size_t i = 0; i < sizeof levels / sizeof levels[0]; i++) {
        if (mkdir(levels[i], 0775) != 0 && errno != EEXIST) {
            ESP_LOGW(TAG, "mkdir %s failed (errno %d)", levels[i], errno);
            return false;
        }
    }
    return true;
}

bool deck_cache_load_manifest(const char *deck_id, deck_manifest_t *out)
{
    char dir[256], path[320];
    if (!out || !deck_dir(deck_id, dir, sizeof dir)) return false;
    snprintf(path, sizeof path, "%s/manifest.json", dir);

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    char *buf = malloc(DECK_MANIFEST_MAX);
    if (!buf) { fclose(f); return false; }
    size_t n = fread(buf, 1, DECK_MANIFEST_MAX, f);
    bool full = feof(f);
    fclose(f);

    bool ok = full && n > 0 && deck_manifest_parse(buf, n, out);
    free(buf);
    if (!ok) ESP_LOGW(TAG, "cached manifest for %s unreadable; ignoring", deck_id);
    return ok;
}

bool deck_cache_save_manifest(const char *deck_id, const char *json, size_t len)
{
    if (!json || !len || len > DECK_MANIFEST_MAX) return false;
    if (!ensure_deck_dir(deck_id)) return false;

    char dir[256], path[320], tmp[320];
    if (!deck_dir(deck_id, dir, sizeof dir)) return false;
    snprintf(tmp, sizeof tmp, "%s/manifest.tmp", dir);
    snprintf(path, sizeof path, "%s/manifest.json", dir);

    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(json, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    if (ok) {
        unlink(path);            /* FATFS rename does not overwrite */
        ok = rename(tmp, path) == 0;
    }
    if (!ok) { unlink(tmp); ESP_LOGW(TAG, "manifest save failed for %s", deck_id); }
    return ok;
}

static bool frame_path(const char *deck_id, const char *digest,
                       char *out, size_t cap)
{
    if (!deck_digest_valid(digest)) return false;
    char dir[256];
    if (!deck_dir(deck_id, dir, sizeof dir)) return false;
    int n = snprintf(out, cap, "%s/%s.bin", dir, digest);
    return n > 0 && n < (int)cap;
}

bool deck_cache_read_frame_ex(const char *deck_id, const char *digest,
                              uint32_t expect_bytes, uint8_t **out,
                              uint8_t **out_blob, size_t *out_blob_len)
{
    if (out_blob) *out_blob = NULL;
    if (out_blob_len) *out_blob_len = 0;
    if (!out) return false;
    *out = NULL;
    if (expect_bytes == 0) return false;

    char path[320];
    if (!frame_path(deck_id, digest, path, sizeof path)) return false;

    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;

    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return false;
    }
    uint64_t max_blob = (uint64_t)expect_bytes + FRAME_CODEC_HEADER_BYTES;
    if (st.st_size <= 0 || (uint64_t)st.st_size > max_blob) {
        close(fd);
        ESP_LOGW(TAG, "cached frame %s invalid file size %lld; deleting",
                 digest, (long long)st.st_size);
        unlink(path);
        return false;
    }

    size_t disk_len = (size_t)st.st_size;
    uint8_t *blob = fb_alloc(disk_len + 1u);
    if (!blob) { close(fd); return false; }

    size_t n = 0;
    bool read_ok = bounce_read_fd(fd, blob, disk_len + 1u, &n);
    close(fd);

    if (!read_ok) {
        ESP_LOGW(TAG, "cached frame %s read failed; keeping it for retry", digest);
        free(blob);
        return false;
    }
    if (n != disk_len) {
        free(blob);
        unlink(path);
        return false;
    }

    /* Fast zero-copy path for legacy unframed raw files on existing SD cards */
    uint8_t *buf = NULL;
    if (n == expect_bytes && !out_blob) {
        buf = blob;
        blob = NULL;
    } else if (n == expect_bytes) {
        buf = fb_alloc(expect_bytes);
        if (!buf) { free(blob); return false; }
        memcpy(buf, blob, expect_bytes);
    } else {
        buf = fb_alloc(expect_bytes);
        if (!buf) { free(blob); return false; }
#ifdef ESP_PLATFORM
        int64_t t_dec0 = esp_timer_get_time();
#endif
        bool dec_ok = frame_codec_decode(blob, n, expect_bytes, buf);
#ifdef ESP_PLATFORM
        int64_t dec_us = esp_timer_get_time() - t_dec0;
#endif
        if (!out_blob) {
            free(blob);
            blob = NULL;
        }
        if (!dec_ok) {
            ESP_LOGW(TAG, "cached frame %s failed decompress; deleting", digest);
            free(blob);
            free(buf);
            unlink(path);
            return false;
        }
#ifdef ESP_PLATFORM
        ESP_LOGI(TAG, "decoded frame %s (%u -> %u bytes in %lld.%02lld ms)",
                 digest, (unsigned)n, (unsigned)expect_bytes,
                 (long long)(dec_us / 1000), (long long)((dec_us % 1000) / 10));
#endif
    }

    if (!deck_digest_check(buf, expect_bytes, expect_bytes, digest)) {
        ESP_LOGW(TAG, "cached frame %s fails verification; deleting", digest);
        free(blob);
        free(buf);
        unlink(path);
        return false;
    }
    if (out_blob) {
        *out_blob = blob;
        if (out_blob_len) *out_blob_len = n;
    } else {
        free(blob);
    }
    *out = buf;
    return true;
}

bool deck_cache_read_frame(const char *deck_id, const char *digest,
                           uint32_t expect_bytes, uint8_t **out)
{
    return deck_cache_read_frame_ex(deck_id, digest, expect_bytes, out, NULL, NULL);
}

bool deck_cache_has_frame(const char *deck_id, const char *digest)
{
    char path[320];
    if (!frame_path(deck_id, digest, path, sizeof path)) return false;
    struct stat st;
    return (stat(path, &st) == 0 && st.st_size > 0);
}

bool deck_cache_write_frame(const char *deck_id, const char *digest,
                            const uint8_t *data, size_t len, uint32_t expect_bytes)
{
    if (!deck_digest_check(data, len, expect_bytes, digest)) {
        ESP_LOGW(TAG, "fetched frame %s fails verification; not caching", digest);
        return false;
    }
    if (!ensure_deck_dir(deck_id)) return false;

    uint8_t *blob = NULL;
    size_t blob_len = 0;
    if (!frame_codec_encode(data, len, &blob, &blob_len)) {
        ESP_LOGW(TAG, "frame encode failed for %s", digest);
        return false;
    }

    char path[320], tmp[320], dir[256];
    if (!frame_path(deck_id, digest, path, sizeof path) ||
        !deck_dir(deck_id, dir, sizeof dir)) {
        free(blob);
        return false;
    }
    snprintf(tmp, sizeof tmp, "%s/frame.tmp", dir);

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        ESP_LOGW(TAG, "frame write: open %s failed (errno %d)", tmp, errno);
        free(blob);
        return false;
    }
    const char *step = "write";
    bool ok = bounce_write_fd(fd, blob, blob_len);
    free(blob);
    int werr = errno;
    if (ok) { step = "close"; ok = (close(fd) == 0); werr = errno; }
    else close(fd);
    if (ok) {
        unlink(path);
        step = "rename";
        ok = rename(tmp, path) == 0;
        werr = errno;
    }
    if (!ok) {
        unlink(tmp);
        ESP_LOGW(TAG, "frame write failed for %s at %s (errno %d, %s -> %s)",
                 digest, step, werr, tmp, path);
    }
    return ok;
}

int deck_cache_list(const char *deck_id,
                    char digests[][DECK_DIGEST_HEX + 1], int max)
{
    char dir[256];
    if (!digests || max <= 0 || !deck_dir(deck_id, dir, sizeof dir)) return 0;

    DIR *d = opendir(dir);
    if (!d) return 0;

    int n = 0;
    struct dirent *e;
    while (n < max && (e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len != DECK_DIGEST_HEX + 4) continue;          /* "<16hex>.bin" */
        if (strcmp(e->d_name + DECK_DIGEST_HEX, ".bin") != 0) continue;
        char digest[DECK_DIGEST_HEX + 1];
        memcpy(digest, e->d_name, DECK_DIGEST_HEX);
        digest[DECK_DIGEST_HEX] = '\0';
        for (int i = 0; i < DECK_DIGEST_HEX; i++)
            if (digest[i] >= 'A' && digest[i] <= 'F') digest[i] += 'a' - 'A';
        if (!deck_digest_valid(digest)) continue;
        strcpy(digests[n++], digest);
    }
    closedir(d);
    return n;
}

void deck_cache_delete(const char *deck_id, const char *digest)
{
    char path[320];
    if (frame_path(deck_id, digest, path, sizeof path)) unlink(path);
}

int32_t deck_cache_frame_age_s(const char *deck_id, const char *digest)
{
    char path[320];
    if (!frame_path(deck_id, digest, path, sizeof path)) return INT32_MAX;
    struct stat st;
    if (stat(path, &st) != 0) return INT32_MAX;
    time_t now = time(NULL);
    if (now < 1600000000 || st.st_mtime < 1600000000 || now < st.st_mtime)
        return 0;   /* uninitialised RTC or FAT timestamp: assume frame is fresh for offline nav */
    return (int32_t)(now - st.st_mtime);
}
