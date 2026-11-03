/*
 * Host-side behavioral tests for Ticket #2 (T1):
 * Unified cache_storage seam + compressed deck_cache & collection_cache.
 */
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cache_storage.h"
#include "collection_cache.h"
#include "deck.h"
#include "deck_cache.h"
#include "frame_codec.h"
#include "frame_collection.h"

/* ---------- Reference FIPS 180-2 SHA-256 (host backend for deck_sha256) ---------- */

typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t fill; } sha_t;

static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha_block(sha_t *s, const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i*4]<<24 | (uint32_t)p[i*4+1]<<16 |
               (uint32_t)p[i*4+2]<<8 | (uint32_t)p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=s->h[0],b=s->h[1],c=s->h[2],d=s->h[3],e=s->h[4],f=s->h[5],g=s->h[6],h=s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    s->h[0]+=a; s->h[1]+=b; s->h[2]+=c; s->h[3]+=d;
    s->h[4]+=e; s->h[5]+=f; s->h[6]+=g; s->h[7]+=h;
}

void deck_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    sha_t s = {.h = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                     0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19},
               .len = (uint64_t)len * 8, .fill = 0};
    while (len >= 64) { sha_block(&s, data); data += 64; len -= 64; }
    memcpy(s.buf, data, len);
    s.fill = len;
    s.buf[s.fill++] = 0x80;
    if (s.fill > 56) { memset(s.buf + s.fill, 0, 64 - s.fill); sha_block(&s, s.buf); s.fill = 0; }
    memset(s.buf + s.fill, 0, 56 - s.fill);
    for (int i = 0; i < 8; i++) s.buf[56 + i] = (uint8_t)(s.len >> (56 - 8 * i));
    sha_block(&s, s.buf);
    for (int i = 0; i < 8; i++) {
        out[i*4]   = (uint8_t)(s.h[i] >> 24);
        out[i*4+1] = (uint8_t)(s.h[i] >> 16);
        out[i*4+2] = (uint8_t)(s.h[i] >> 8);
        out[i*4+3] = (uint8_t)(s.h[i]);
    }
}

static int tests = 0, fails = 0;
#define CHECK(cond) do { tests++; if (!(cond)) { fails++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define E1003_FRAME_BYTES 1314144u

static void rm_tree(const char *path)
{
    DIR *d = opendir(path);
    if (!d) { unlink(path); return; }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char sub[512];
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode)) rm_tree(sub);
        else unlink(sub);
    }
    closedir(d);
    rmdir(path);
}

/* Fill a synthetic 1872x1404 4bpp dashboard frame (white background, header bar,
 * grayscale ramp cards, and simulated glyph runs). */
static void make_dashboard_frame(uint8_t *buf, size_t n)
{
    memset(buf, 0xFF, n);
    /* Top header bar (64 rows of dark gray 0x22) */
    memset(buf, 0x22, 936 * 64);
    /* Content cards + simulated text runs */
    for (int y = 120; y < 1300; y++) {
        uint8_t *row = buf + (size_t)y * 936;
        if ((y % 180) < 4) {
            memset(row + 32, 0x44, 872);
        } else if ((y % 28) < 12) {
            for (int x = 48; x < 820; x += 24) {
                uint8_t tone = (uint8_t)(((x + y) / 16) & 0x0F);
                uint8_t byte_val = (uint8_t)((tone << 4) | tone);
                memset(row + x, byte_val, 14);
            }
        }
    }
}

static double monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void test_flash_cache_deck_roundtrip_and_speed(const char *tmp_root)
{
    cache_storage_set_test_root(tmp_root, 8000000ULL, CACHE_BACKEND_FLASH);
    CHECK(cache_storage_mounted());
    CHECK(cache_storage_backend() == CACHE_BACKEND_FLASH);

    static const char MANIFEST_JSON[] =
        "{\"deck_id\":\"qemu_e1003_deck\",\"version\":\"0123456789abcdef\","
        "\"entry_page_id\":\"card1\",\"pages\":["
        "{\"page_id\":\"card1\",\"digest\":\"00112233445566aa\",\"bytes\":1314144,"
        "\"ttl_s\":86400,\"links\":[]}]}";

    CHECK(deck_cache_save_manifest("qemu_e1003_deck", MANIFEST_JSON, sizeof(MANIFEST_JSON) - 1));
    deck_manifest_t loaded;
    CHECK(deck_cache_load_manifest("qemu_e1003_deck", &loaded));
    CHECK(strcmp(loaded.deck_id, "qemu_e1003_deck") == 0);
    CHECK(loaded.n_pages == 1);

    uint8_t *raw = malloc(E1003_FRAME_BYTES);
    CHECK(raw != NULL);
    make_dashboard_frame(raw, E1003_FRAME_BYTES);

    uint8_t sha[32];
    char digest[DECK_DIGEST_HEX + 1];
    deck_sha256(raw, E1003_FRAME_BYTES, sha);
    deck_digest_hex16(sha, digest);

    CHECK(deck_cache_write_frame("qemu_e1003_deck", digest, raw, E1003_FRAME_BYTES, E1003_FRAME_BYTES));

    /* Verify on-disk representation is genuinely compressed (< 25% of 1.31 MB). */
    char bin_path[512];
    snprintf(bin_path, sizeof bin_path, "%s/tesserae/decks/qemu_e1003_deck/%s.bin", tmp_root, digest);
    struct stat st;
    CHECK(stat(bin_path, &st) == 0);
    CHECK(st.st_size > 16 && (size_t)st.st_size < E1003_FRAME_BYTES / 4);

    /* Verify decode + SHA-256 check completes in < 25 ms and matches byte-for-byte. */
    uint8_t *decoded = NULL;
    double t0 = monotonic_ms();
    bool ok = deck_cache_read_frame("qemu_e1003_deck", digest, E1003_FRAME_BYTES, &decoded);
    double elapsed_ms = monotonic_ms() - t0;

    CHECK(ok);
    CHECK(decoded != NULL);
    CHECK(elapsed_ms < 25.0);
    if (decoded) {
        CHECK(memcmp(raw, decoded, E1003_FRAME_BYTES) == 0);
        free(decoded);
    }
    free(raw);
}

static void test_corruption_eviction_legacy_and_collections(const char *tmp_root)
{
    cache_storage_set_test_root(tmp_root, 8000000ULL, CACHE_BACKEND_FLASH);

    /* 1. Legacy unframed raw SD file whose top-left 4bpp pixels happen to spell
     * "TSZ1" (0x54, 0x53, 0x5A, 0x31) must still read + SHA-256-verify cleanly. */
    const uint32_t small_panel = 48000u;
    uint8_t *legacy = malloc(small_panel);
    CHECK(legacy != NULL);
    for (uint32_t i = 0; i < small_panel; i++) legacy[i] = (uint8_t)((i * 73u + 19u) & 0xFFu);
    memcpy(legacy, "TSZ1", 4);

    uint8_t sha[32];
    char leg_digest[DECK_DIGEST_HEX + 1];
    deck_sha256(legacy, small_panel, sha);
    deck_digest_hex16(sha, leg_digest);

    /* Write unframed raw bytes directly as an older firmware build would have. */
    char leg_path[512];
    snprintf(leg_path, sizeof leg_path, "%s/tesserae/decks/qemu_e1003_deck/%s.bin",
             tmp_root, leg_digest);
    FILE *lf = fopen(leg_path, "wb");
    CHECK(lf != NULL);
    if (lf) {
        CHECK(fwrite(legacy, 1, small_panel, lf) == small_panel);
        fclose(lf);
    }

    uint8_t *leg_out = NULL;
    CHECK(deck_cache_read_frame("qemu_e1003_deck", leg_digest, small_panel, &leg_out));
    CHECK(leg_out != NULL && memcmp(legacy, leg_out, small_panel) == 0);
    free(leg_out);
    free(legacy);

    /* 2. Incompressible pseudo-random frame falls back to TSZ1 RAW envelope and round-trips. */
    uint8_t *noisy = malloc(small_panel);
    CHECK(noisy != NULL);
    uint32_t rng = 0xCAFEBABEu;
    for (uint32_t i = 0; i < small_panel; i++) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        noisy[i] = (uint8_t)(rng & 0xFFu);
    }
    char noisy_digest[DECK_DIGEST_HEX + 1];
    deck_sha256(noisy, small_panel, sha);
    deck_digest_hex16(sha, noisy_digest);

    CHECK(deck_cache_write_frame("qemu_e1003_deck", noisy_digest, noisy, small_panel, small_panel));
    uint8_t *noisy_out = NULL;
    CHECK(deck_cache_read_frame("qemu_e1003_deck", noisy_digest, small_panel, &noisy_out));
    CHECK(noisy_out != NULL && memcmp(noisy, noisy_out, small_panel) == 0);
    free(noisy_out);

    /* 3. Flipping a byte in a cached .bin fails verification AND evicts the file. */
    char noisy_path[512];
    snprintf(noisy_path, sizeof noisy_path, "%s/tesserae/decks/qemu_e1003_deck/%s.bin",
             tmp_root, noisy_digest);
    FILE *cf = fopen(noisy_path, "r+b");
    CHECK(cf != NULL);
    if (cf) {
        fseek(cf, 24, SEEK_SET);
        uint8_t b = 0;
        CHECK(fread(&b, 1, 1, cf) == 1);
        b ^= 0xFFu;
        fseek(cf, 24, SEEK_SET);
        CHECK(fwrite(&b, 1, 1, cf) == 1);
        fclose(cf);
    }
    uint8_t *bad_out = NULL;
    CHECK(!deck_cache_read_frame("qemu_e1003_deck", noisy_digest, small_panel, &bad_out));
    CHECK(bad_out == NULL);
    struct stat st;
    CHECK(stat(noisy_path, &st) != 0); /* evicted */
    free(noisy);

    /* 4. Collection cache compressed round-trip, listing, truncation eviction & capacity scaling. */
    static const char COLL_JSON[] =
        "{\"schema\":1,\"collection_id\":\"album:gallery\",\"kind\":\"album\","
        "\"version\":\"a1b2c3d4e5f60718\",\"total_frames\":1,"
        "\"cursor\":null,\"next_cursor\":null,\"frames\":["
        "{\"frame_id\":\"photo:1\",\"position\":0,\"digest\":\"0011223344556677\","
        "\"bytes\":1314144,\"ttl_s\":0,\"cache\":true,"
        "\"url\":\"/api/v1/device/e1003/collection/frame/0011223344556677\"}],"
        "\"producer\":{\"album\":{\"playback\":{\"mode\":\"sequential\",\"interval_s\":1800,\"repeat\":\"loop\"}}}}";

    CHECK(collection_cache_save_manifest("album:gallery", COLL_JSON, sizeof(COLL_JSON) - 1));
    fc_manifest_t cm;
    CHECK(collection_cache_load_manifest("album:gallery", &cm));
    CHECK(cm.n_frames == 1);

    uint8_t *art = malloc(E1003_FRAME_BYTES);
    CHECK(art != NULL);
    make_dashboard_frame(art, E1003_FRAME_BYTES);
    art[500] = 0x3C;
    char art_digest[FC_DIGEST_HEX + 1];
    deck_sha256(art, E1003_FRAME_BYTES, sha);
    deck_digest_hex16(sha, art_digest);

    CHECK(collection_cache_write_frame("album:gallery", art_digest, art, E1003_FRAME_BYTES, E1003_FRAME_BYTES));
    char listed[4][FC_DIGEST_HEX + 1];
    CHECK(collection_cache_list("album:gallery", listed, 4) == 1);
    CHECK(strcmp(listed[0], art_digest) == 0);

    uint8_t *art_back = NULL;
    CHECK(collection_cache_read_frame("album:gallery", art_digest, E1003_FRAME_BYTES, &art_back));
    CHECK(art_back != NULL && memcmp(art, art_back, E1003_FRAME_BYTES) == 0);
    free(art_back);
    free(art);

    /* Effective capacity scales 4x when >= 2 raw frames free, and clamps 1:1 when tight. */
    cache_storage_set_test_root(tmp_root, 8000000ULL, CACHE_BACKEND_FLASH);
    CHECK(cache_storage_effective_capacity_bytes(E1003_FRAME_BYTES) == 32000000ULL);
    cache_storage_set_test_root(tmp_root, 2000000ULL, CACHE_BACKEND_FLASH);
    CHECK(cache_storage_effective_capacity_bytes(E1003_FRAME_BYTES) == 2000000ULL);

    /* Unmounted state fails soft with 0 capacity. */
    cache_storage_unmount();
    CHECK(!cache_storage_mounted());
    CHECK(cache_storage_effective_capacity_bytes(E1003_FRAME_BYTES) == 0ULL);
    deck_manifest_t dummy_m;
    CHECK(!deck_cache_load_manifest("qemu_e1003_deck", &dummy_m));

    /* 5. Hostile / malformed LZ4 streams are rejected safely without UB or overrun. */
    uint8_t out_small[64];
    /* Zero match offset (illegal in LZ4): 1 literal 'A', then offset 0x0000 */
    const uint8_t bad_zero_off[] = {
        'T','S','Z','1', 1, 1, 0, 0,
        64, 0, 0, 0,
        4,  0, 0, 0,
        0x10, 'A', 0x00, 0x00
    };
    CHECK(!frame_codec_decode(bad_zero_off, sizeof(bad_zero_off), 64, out_small));

    /* Offset larger than decoded output so far (offset = 10 after 1 literal) */
    const uint8_t bad_oob_off[] = {
        'T','S','Z','1', 1, 1, 0, 0,
        64, 0, 0, 0,
        4,  0, 0, 0,
        0x10, 'A', 0x0A, 0x00
    };
    CHECK(!frame_codec_decode(bad_oob_off, sizeof(bad_oob_off), 64, out_small));

    /* Match length overflowing dst_raw capacity (1 literal + match_len 15+255+4 > 64) */
    const uint8_t bad_overflow_match[] = {
        'T','S','Z','1', 1, 1, 0, 0,
        64, 0, 0, 0,
        6,  0, 0, 0,
        0x1F, 'A', 0x01, 0x00, 0xFF, 0x10
    };
    CHECK(!frame_codec_decode(bad_overflow_match, sizeof(bad_overflow_match), 64, out_small));
}

int main(void)
{
    char tmpl[] = "/tmp/tesserae-fc-test.XXXXXX";
    char *dir = mkdtemp(tmpl);
    CHECK(dir != NULL);
    if (dir) {
        test_flash_cache_deck_roundtrip_and_speed(dir);
        test_corruption_eviction_legacy_and_collections(dir);
        cache_storage_unmount();
        rm_tree(dir);
    }

    printf("flash_cache: %d/%d passed\n", tests - fails, tests);
    return fails ? 1 : 0;
}
