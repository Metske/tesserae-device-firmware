#!/bin/sh
# Host unit test runner for FreeInkUI Widget Region & status_strip (Ticket #6 / T5).
set -eu

PIO_CORE_DIR="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}"
CJSON_DIR="$PIO_CORE_DIR/packages/framework-espidf/components/json/cJSON"
if [ ! -f "$CJSON_DIR/cJSON.c" ]; then
    echo "ESP-IDF cJSON source not found at $CJSON_DIR" >&2
    exit 1
fi

OUT_CJSON="$(mktemp -t tesserae-cjson.XXXXXX.o)"
OUT="$(mktemp -t tesserae-widget.XXXXXX)"

cc -std=c11 -O2 -I "$CJSON_DIR" -c "$CJSON_DIR/cJSON.c" -o "$OUT_CJSON"

g++ -std=c++17 -O2 -Wall -Wextra \
    -I src \
    -I include \
    -I "$CJSON_DIR" \
    -I src/vendor/freeink \
    -I src/vendor/freeink/FreeInkUI/include \
    -I src/vendor/freeink/FreeInkFont/include \
    -I src/vendor/freeink/FreeInkFont/third_party/stb \
    test/test_widget_status_strip.cpp \
    src/freeink_compositor.cpp \
    src/widget_region.cpp \
    src/vendor/freeink/FreeInkUI/src/FreeInkUI.cpp \
    src/vendor/freeink/FreeInkFont/src/TtfFont.cpp \
    "$OUT_CJSON" \
    -lm -o "$OUT"

rc=0
"$OUT" || rc=$?

rm -f "$OUT" "$OUT_CJSON"
exit $rc
