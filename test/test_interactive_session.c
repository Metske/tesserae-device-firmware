/*
 * test_interactive_session.c -- host behavioral tests for Ticket #3 (T2):
 *   1. Unified button & horizontal swipe navigation over Deck and Collection
 *   2. Input coalescing while Display Worker is busy (in-flight Fast/Settle)
 *   3. Fast Pass -> ~2.5 s Settle Pass -> ~30 s Sleep timer transitions
 *   4. PSRAM Neighbor Prefetch scheduling, cache hits, and bounded eviction
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "interactive_session.h"
#include "frame_codec.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { \
        g_pass++; \
    } else { \
        g_fail++; \
        fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, (msg)); \
    } \
} while (0)

/* Reference stub required by deck.c / frame_collection.c when linked on host
 * (manifest parsing and navigation do not call deck_sha256_hex16). */
void deck_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    (void)data; (void)len;
    memset(out, 0, 32);
}

static void make_three_page_deck(deck_manifest_t *m)
{
    const char *json =
        "{"
        "  \"deck_id\": \"home_deck\","
        "  \"version\": \"v1\","
        "  \"entry_page_id\": \"p1\","
        "  \"pages\": ["
        "    {\"page_id\": \"p1\", \"digest\": \"1111111111111111\", \"bytes\": 4096, \"ttl_s\": 0,"
        "     \"links\": ["
        "       {\"button\": \"right\", \"target_page_id\": \"p2\"},"
        "       {\"button\": \"left\",  \"target_page_id\": \"p3\"},"
        "       {\"zone\": {\"x\": 0.8, \"y\": 0.8, \"w\": 0.2, \"h\": 0.2}, \"target_page_id\": \"p3\"}"
        "     ]},"
        "    {\"page_id\": \"p2\", \"digest\": \"2222222222222222\", \"bytes\": 4096, \"ttl_s\": 0,"
        "     \"links\": ["
        "       {\"button\": \"right\", \"target_page_id\": \"p3\"},"
        "       {\"button\": \"left\",  \"target_page_id\": \"p1\"}"
        "     ]},"
        "    {\"page_id\": \"p3\", \"digest\": \"3333333333333333\", \"bytes\": 4096, \"ttl_s\": 0,"
        "     \"links\": ["
        "       {\"button\": \"right\", \"target_page_id\": \"p1\"},"
        "       {\"button\": \"left\",  \"target_page_id\": \"p2\"}"
        "     ]}"
        "  ]"
        "}";
    CHECK(deck_manifest_parse(json, strlen(json), m), "parse 3-page deck");
}

static void make_three_frame_collection(fc_manifest_t *fc)
{
    const char *json =
        "{"
        "  \"schema\": 1,"
        "  \"collection_id\": \"gallery_01\","
        "  \"kind\": \"album\","
        "  \"version\": \"a1b2c3d4e5f60718\","
        "  \"total_frames\": 3,"
        "  \"cursor\": null,"
        "  \"next_cursor\": null,"
        "  \"producer\": {\"album\": {\"playback\": {\"mode\": \"sequential\", \"repeat\": \"loop\", \"interval_s\": 300}}},"
        "  \"frames\": ["
        "    {\"frame_id\": \"f0\", \"position\": 0, \"cache\": true, \"digest\": \"aaaa000000000000\", \"bytes\": 4096, \"ttl_s\": 0, \"url\": \"/f0\"},"
        "    {\"frame_id\": \"f1\", \"position\": 1, \"cache\": true, \"digest\": \"bbbb000000000000\", \"bytes\": 4096, \"ttl_s\": 0, \"url\": \"/f1\"},"
        "    {\"frame_id\": \"f2\", \"position\": 2, \"cache\": true, \"digest\": \"cccc000000000000\", \"bytes\": 4096, \"ttl_s\": 0, \"url\": \"/f2\"}"
        "  ]"
        "}";
    CHECK(fc_manifest_parse(json, strlen(json), fc), "parse 3-frame collection");
}

/* Slice 1: Unified button & horizontal touch swipe navigation on Deck & Collection. */
static void test_unified_navigation_deck_and_collection(void)
{
    deck_manifest_t deck;
    make_three_page_deck(&deck);

    interactive_session_t s;
    isess_init_deck(&s, &deck, "p1", 1872, 1404, 1000);
    CHECK(strcmp(s.displayed_id, "p1") == 0, "deck starts on p1");
    CHECK(strcmp(s.target_id, "p1") == 0, "deck target starts on p1");

    /* Right button -> p2 */
    CHECK(isess_on_button(&s, "right", 1200) == ISESS_INPUT_HANDLED_LOCAL,
          "right button handled locally");
    CHECK(strcmp(s.target_id, "p2") == 0, "right button moves target to p2");
    CHECK(s.last_input_ms == 1200, "button updates last_input_ms");

    isess_work_t w;
    CHECK(isess_next_work(&s, 1205, &w) && w.kind == ISESS_WORK_FAST_PASS,
          "worker receives FAST_PASS for p2");
    CHECK(strcmp(w.item_id, "p2") == 0 && strcmp(w.digest, "2222222222222222") == 0,
          "FAST_PASS carries p2 id and digest");
    isess_complete_work(&s, &w, true, 1640);
    CHECK(strcmp(s.displayed_id, "p2") == 0, "p2 now displayed");

    /* Horizontal swipe left (1400,702 -> 472,702) -> next ("right") -> p3 */
    CHECK(isess_on_touch(&s, 1400, 702, 472, 702, 240, 2000) == ISESS_INPUT_HANDLED_LOCAL,
          "swipe left maps to next page p3");
    CHECK(strcmp(s.target_id, "p3") == 0, "target moved to p3");
    CHECK(isess_next_work(&s, 2010, &w) && w.kind == ISESS_WORK_FAST_PASS,
          "FAST_PASS scheduled for swipe-left to p3");
    isess_complete_work(&s, &w, true, 2450);

    /* Horizontal swipe right (472,702 -> 1400,702) -> prev ("left") -> p2 */
    CHECK(isess_on_touch(&s, 472, 702, 1400, 702, 240, 2600) == ISESS_INPUT_HANDLED_LOCAL,
          "swipe right maps to prev page p2");
    CHECK(strcmp(s.target_id, "p2") == 0, "target moved back to p2");
    CHECK(isess_next_work(&s, 2605, &w) && w.kind == ISESS_WORK_FAST_PASS,
          "FAST_PASS scheduled for swipe-right to p2");
    isess_complete_work(&s, &w, true, 3040);

    /* Return to p1, then test point-zone tap (bottom-right corner -> p3) vs center miss */
    CHECK(isess_on_button(&s, "left", 3100) == ISESS_INPUT_HANDLED_LOCAL, "left -> p1");
    CHECK(isess_next_work(&s, 3105, &w) && w.kind == ISESS_WORK_FAST_PASS, "fast p1");
    isess_complete_work(&s, &w, true, 3540);

    CHECK(isess_on_touch(&s, 936, 702, 936, 702, 80, 3600) == ISESS_INPUT_UNHANDLED,
          "center tap with no zone link returns UNHANDLED");
    CHECK(s.last_input_ms == 3600, "unhandled tap still resets inactivity timer");

    CHECK(isess_on_touch(&s, 1700, 1300, 1700, 1300, 80, 3700) == ISESS_INPUT_HANDLED_LOCAL,
          "bottom-right zone tap hits p3 link");
    CHECK(strcmp(s.target_id, "p3") == 0, "zone tap moved target to p3");
    isess_free(&s);

    /* Collection navigation: wrap-around prev/next via button and swipe */
    fc_manifest_t fc;
    make_three_frame_collection(&fc);
    isess_init_collection(&s, &fc, 0, 1872, 1404, 5000);
    CHECK(strcmp(s.displayed_id, "f0") == 0, "collection starts on f0");

    CHECK(isess_on_button(&s, "left", 5100) == ISESS_INPUT_HANDLED_LOCAL,
          "collection left wraps 0 -> 2");
    CHECK(s.target_index == 2 && strcmp(s.target_id, "f2") == 0,
          "collection target wrapped to f2");
    CHECK(isess_on_touch(&s, 1400, 702, 472, 702, 200, 5200) == ISESS_INPUT_HANDLED_LOCAL,
          "collection swipe-left wraps 2 -> 0");
    CHECK(s.target_index == 0, "collection target wrapped back to f0");
    isess_free(&s);
}

/* Slice 2: Input coalescing while worker is busy + Fast -> Settle -> Sleep timers. */
static void test_coalescing_and_settle_and_sleep_timers(void)
{
    deck_manifest_t deck;
    make_three_page_deck(&deck);

    interactive_session_t s;
    isess_init_deck(&s, &deck, "p1", 1872, 1404, 1000);

    /* Start FAST_PASS to p2 */
    CHECK(isess_on_button(&s, "right", 1100) == ISESS_INPUT_HANDLED_LOCAL, "p1->p2");
    isess_work_t w_fast;
    CHECK(isess_next_work(&s, 1105, &w_fast) && w_fast.kind == ISESS_WORK_FAST_PASS,
          "dispatch FAST_PASS p2");

    /* While worker is busy on p2, user rapidly presses right (p3) then right (p1) then left (p3) */
    CHECK(isess_on_button(&s, "right", 1150) == ISESS_INPUT_HANDLED_LOCAL, "busy p2->p3");
    CHECK(!isess_next_work(&s, 1160, &w_fast), "no new work while worker busy");
    CHECK(isess_on_button(&s, "right", 1200) == ISESS_INPUT_HANDLED_LOCAL, "busy p3->p1");
    CHECK(isess_on_button(&s, "left",  1250) == ISESS_INPUT_HANDLED_LOCAL, "busy p1->p3");
    CHECK(strcmp(s.target_id, "p3") == 0, "rapid inputs coalesced to final target p3");

    /* Complete in-flight p2 FAST_PASS -> worker immediately picks up single coalesced p3 FAST_PASS */
    w_fast.kind = ISESS_WORK_FAST_PASS;
    snprintf(w_fast.item_id, sizeof(w_fast.item_id), "p2");
    snprintf(w_fast.digest, sizeof(w_fast.digest), "2222222222222222");
    w_fast.rx = 0; w_fast.ry = 0; w_fast.rw = 1872; w_fast.rh = 1404;
    isess_complete_work(&s, &w_fast, true, 1540);

    isess_work_t w2;
    CHECK(isess_next_work(&s, 1545, &w2) && w2.kind == ISESS_WORK_FAST_PASS &&
          strcmp(w2.item_id, "p3") == 0,
          "single coalesced FAST_PASS emitted for p3 (skipping intermediate p1)");

    /* While p3 FAST_PASS is in flight, user flips right (p1) and back left (p3):
     * returning to p3 cancels redundant extra FAST_PASS! */
    CHECK(isess_on_button(&s, "right", 1600) == ISESS_INPUT_HANDLED_LOCAL, "p3->p1");
    CHECK(isess_on_button(&s, "left",  1650) == ISESS_INPUT_HANDLED_LOCAL, "p1->p3");
    isess_complete_work(&s, &w2, true, 2000);
    CHECK(strcmp(s.displayed_id, "p3") == 0, "p3 displayed");
    CHECK(s.needs_settle, "FAST_PASS marks session dirty for Settle Pass");

    /* Before 2500 ms pause (last_fast_done=2000, last_input=1650), no SETTLE_PASS yet */
    isess_work_t w_idle;
    bool got = isess_next_work(&s, 4490, &w_idle);
    CHECK(!got || w_idle.kind == ISESS_WORK_PREFETCH,
          "before 2500 ms idle, SETTLE_PASS does not fire early");
    if (got) isess_complete_work(&s, &w_idle, false, 4495);

    /* At t = 4500 ms (2500 ms after fast pass done), SETTLE_PASS fires on full frame */
    isess_work_t w_settle;
    CHECK(isess_next_work(&s, 4500, &w_settle) && w_settle.kind == ISESS_WORK_SETTLE_PASS,
          "SETTLE_PASS fires after 2.5 s pause");
    CHECK(w_settle.rx == 0 && w_settle.ry == 0 &&
          w_settle.rw == 1872 && w_settle.rh == 1404,
          "full-page flip settles full 1872x1404 rect");
    isess_complete_work(&s, &w_settle, true, 6064);
    CHECK(!s.needs_settle, "SETTLE_PASS clears dirty state");

    /* Partial fast dirty rect alignment + union + pre-sleep mandatory settle */
    isess_mark_dirty_rect(&s, 101, 40, 299, 60, 10000);
    CHECK(s.needs_settle, "mark_dirty_rect sets needs_settle");
    CHECK(s.dirty_x == 100 && s.dirty_w == 300,
          "dirty rect 101..400 widens to 4-px aligned 100..400 (w=300)");
    isess_mark_dirty_rect(&s, 350, 80, 53, 40, 10100);
    CHECK(s.dirty_x == 100 && s.dirty_y == 40 &&
          s.dirty_w == 304 && s.dirty_h == 80,
          "second dirty rect unions cleanly to 100..404 x 40..120");

    /* Jump past 30 s inactivity timeout (10100 + 30000 = 40100) while still dirty:
     * MUST emit SETTLE_PASS first, then SLEEP! */
    isess_work_t w_exp;
    CHECK(isess_next_work(&s, 40100, &w_exp) && w_exp.kind == ISESS_WORK_SETTLE_PASS,
          "session expiry runs mandatory GC16 SETTLE_PASS before SLEEP");
    CHECK(w_exp.rx == 100 && w_exp.ry == 40 && w_exp.rw == 304 && w_exp.rh == 80,
          "expiry SETTLE_PASS targets unioned dirty rect");
    isess_complete_work(&s, &w_exp, true, 41664);

    CHECK(isess_next_work(&s, 41665, &w_exp) && w_exp.kind == ISESS_WORK_SLEEP,
          "after clean settle, expired session emits SLEEP");
    isess_complete_work(&s, &w_exp, true, 41670);
    CHECK(s.expired, "session marked expired after SLEEP");
    isess_free(&s);
}

#include "cJSON.h"

static void make_ten_frame_collection(fc_manifest_t *fc)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "schema", 1);
    cJSON_AddStringToObject(root, "collection_id", "gallery_10");
    cJSON_AddStringToObject(root, "kind", "album");
    cJSON_AddStringToObject(root, "version", "0123456789abcdef");
    cJSON_AddNumberToObject(root, "total_frames", 10);
    cJSON_AddNullToObject(root, "cursor");
    cJSON_AddNullToObject(root, "next_cursor");
    cJSON *prod = cJSON_AddObjectToObject(root, "producer");
    cJSON *alb = cJSON_AddObjectToObject(prod, "album");
    cJSON *pb = cJSON_AddObjectToObject(alb, "playback");
    cJSON_AddStringToObject(pb, "mode", "sequential");
    cJSON_AddStringToObject(pb, "repeat", "loop");
    cJSON_AddNumberToObject(pb, "interval_s", 300);

    cJSON *frames = cJSON_AddArrayToObject(root, "frames");
    for (int i = 0; i < 10; i++) {
        char id[16], dig[17], url[32];
        snprintf(id, sizeof(id), "f%d", i);
        snprintf(dig, sizeof(dig), "%016x", 0x1000 + i);
        snprintf(url, sizeof(url), "/frames/%d", i);
        cJSON *f = cJSON_CreateObject();
        cJSON_AddStringToObject(f, "frame_id", id);
        cJSON_AddNumberToObject(f, "position", i);
        cJSON_AddBoolToObject(f, "cache", true);
        cJSON_AddStringToObject(f, "digest", dig);
        cJSON_AddNumberToObject(f, "bytes", 2048);
        cJSON_AddNumberToObject(f, "ttl_s", 0);
        cJSON_AddStringToObject(f, "url", url);
        cJSON_AddItemToArray(frames, f);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    CHECK(fc_manifest_parse(json, strlen(json), fc), "parse 10-frame collection");
    free(json);
}

/* Slice 3: PSRAM Neighbor Prefetch scheduling, TSZ1 decode hit, and bounded eviction. */
static void test_neighbor_prefetch_hit_and_eviction(void)
{
    fc_manifest_t fc;
    make_ten_frame_collection(&fc);

    interactive_session_t s;
    isess_init_collection(&s, &fc, 0, 1872, 1404, 1000);

    /* Idle on f0: worker receives PREFETCH for next (f1) then prev (f9) */
    isess_work_t w;
    CHECK(isess_next_work(&s, 1010, &w) && w.kind == ISESS_WORK_PREFETCH,
          "idle worker schedules first neighbor prefetch");
    CHECK(strcmp(w.item_id, "f1") == 0 && w.nav_dir == ISESS_DIR_NEXT,
          "first neighbor prefetch is next frame f1");

    /* Encode a synthetic 2048-byte frame into TSZ1 and store in Neighbor Prefetch */
    uint8_t raw_f1[2048];
    for (size_t i = 0; i < sizeof(raw_f1); i++) raw_f1[i] = (uint8_t)((i / 32) ^ 0x5A);
    uint8_t *comp = NULL;
    size_t comp_len = 0;
    CHECK(frame_codec_encode(raw_f1, sizeof(raw_f1), &comp, &comp_len),
          "encode f1 into TSZ1");
    CHECK(isess_prefetch_put(&s, w.digest, comp, comp_len, sizeof(raw_f1)),
          "store f1 TSZ1 blob in PSRAM prefetch cache");
    free(comp);
    isess_complete_work(&s, &w, true, 1030);
    CHECK(isess_prefetch_has(&s, fc.frames[1].digest), "f1 is in prefetch cache");

    /* Next idle poll schedules prev neighbor (f9), and failed load skips retry loop */
    CHECK(isess_next_work(&s, 1040, &w) && w.kind == ISESS_WORK_PREFETCH &&
          strcmp(w.item_id, "f9") == 0 && w.nav_dir == ISESS_DIR_PREV,
          "second neighbor prefetch is prev frame f9");
    isess_complete_work(&s, &w, false, 1050);
    CHECK(!isess_next_work(&s, 1060, &w),
          "failed prefetch is skipped for current frame instead of busy-looping");

    /* Decoding f1 from prefetch hits PSRAM and matches byte-for-byte */
    uint8_t *decoded = NULL;
    CHECK(isess_prefetch_decode(&s, fc.frames[1].digest, sizeof(raw_f1), &decoded),
          "decode f1 from Neighbor Prefetch");
    CHECK(decoded != NULL && memcmp(decoded, raw_f1, sizeof(raw_f1)) == 0,
          "decoded Neighbor Prefetch frame matches exact pixels");
    CHECK(s.prefetch_hits == 1 && s.prefetch_misses == 0,
          "prefetch hit counter incremented");
    free(decoded);

    /* Walk through all 10 frames, populating > ISESS_MAX_PREFETCH (8) blobs:
     * distant frames are evicted while immediate neighbors of current frame stay hot. */
    for (int step = 0; step < 9; step++) {
        CHECK(isess_on_button(&s, "right", 2000 + step * 600) == ISESS_INPUT_HANDLED_LOCAL,
              "advance collection");
        CHECK(isess_next_work(&s, 2005 + step * 600, &w) && w.kind == ISESS_WORK_FAST_PASS,
              "fast pass for step");
        isess_complete_work(&s, &w, true, 2440 + step * 600);

        /* Populate current + next + prev into prefetch */
        int cur = s.displayed_index;
        int idxs[3] = { cur, (cur + 1) % 10, (cur + 9) % 10 };
        for (int k = 0; k < 3; k++) {
            const fc_frame_t *fr = &fc.frames[idxs[k]];
            uint8_t dummy[2048];
            memset(dummy, (uint8_t)idxs[k], sizeof(dummy));
            CHECK(isess_prefetch_put(&s, fr->digest, dummy, sizeof(dummy), sizeof(dummy)),
                  "put neighbor in bounded prefetch cache");
        }
    }
    /* Now at f9: f9, f0 (next), and f8 (prev) MUST still be resident, and evictions > 0 */
    CHECK(s.displayed_index == 9, "walked to f9");
    CHECK(s.prefetch_evictions >= 2, "bounded 8-slot cache evicted distant frames");
    CHECK(isess_prefetch_has(&s, fc.frames[9].digest), "current frame f9 kept");
    CHECK(isess_prefetch_has(&s, fc.frames[0].digest), "next neighbor f0 kept");
    CHECK(isess_prefetch_has(&s, fc.frames[8].digest), "prev neighbor f8 kept");

    isess_free(&s);
}

int main(void)
{
    test_unified_navigation_deck_and_collection();
    test_coalescing_and_settle_and_sleep_timers();
    test_neighbor_prefetch_hit_and_eviction();
    printf("interactive_session tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
