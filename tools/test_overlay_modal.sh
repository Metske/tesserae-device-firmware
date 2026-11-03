#!/bin/sh
# Host unit test runner for FreeInkUI 4-bpp compositor & Overlay Modal (Ticket #4 / T3).
set -eu

PIO_CORE_DIR="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}"
CJSON_DIR="$PIO_CORE_DIR/packages/framework-espidf/components/json/cJSON"
if [ ! -f "$CJSON_DIR/cJSON.c" ]; then
    echo "ESP-IDF cJSON source not found at $CJSON_DIR" >&2
    exit 1
fi

OUT_CJSON="$(mktemp -t tesserae-cjson.XXXXXX.o)"
OUT="$(mktemp -t tesserae-omodal.XXXXXX)"
TTF_FIXTURE="/home/owenh/vcodex-wiki/lib/EpdFont/builtinFonts/source/NotoSans/NotoSans-Regular.ttf"

cc -std=c11 -O2 -I "$CJSON_DIR" -c "$CJSON_DIR/cJSON.c" -o "$OUT_CJSON"
OUT_DECK="$(mktemp -t tesserae-deck.XXXXXX.o)"
OUT_FC="$(mktemp -t tesserae-fc.XXXXXX.o)"
cc -std=c11 -O2 -I src -I include -I "$CJSON_DIR" -c src/deck.c -o "$OUT_DECK"
cc -std=c11 -O2 -I src -I include -I "$CJSON_DIR" -c src/frame_collection.c -o "$OUT_FC"

g++ -std=c++17 -O2 -Wall -Wextra \
    -I src \
    -I include \
    -I "$CJSON_DIR" \
    -I src/vendor/freeink \
    -I src/vendor/freeink/FreeInkUI/include \
    -I src/vendor/freeink/FreeInkFont/include \
    -I src/vendor/freeink/FreeInkFont/third_party/stb \
    test/test_overlay_modal.cpp \
    src/freeink_compositor.cpp \
    src/overlay_modal.cpp \
    src/vendor/freeink/FreeInkUI/src/FreeInkUI.cpp \
    src/vendor/freeink/FreeInkFont/src/TtfFont.cpp \
    "$OUT_DECK" \
    "$OUT_FC" \
    "$OUT_CJSON" \
    -lm -o "$OUT"

rc=0
if [ -f "$TTF_FIXTURE" ]; then
    "$OUT" "$TTF_FIXTURE" || rc=$?
else
    "$OUT" || rc=$?
fi

rm -f "$OUT" "$OUT_CJSON" "$OUT_DECK" "$OUT_FC"
exit $rc
