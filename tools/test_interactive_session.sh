#!/bin/sh
# Host-side behavioral tests for the unified Interactive Session state machine,
# input coalescing, PSRAM Neighbor Prefetch cache, and Fast->Settle->Sleep worker.
set -eu

PIO_CORE_DIR="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}"
CJSON_DIR="$PIO_CORE_DIR/packages/framework-espidf/components/json/cJSON"
if [ ! -f "$CJSON_DIR/cJSON.c" ]; then
    echo "ESP-IDF cJSON source not found at $CJSON_DIR" >&2
    exit 1
fi

OUT="$(mktemp -t tesserae-isess.XXXXXX)"
cc -std=c11 -O2 -Wall -Wextra -Werror \
   -I src \
   -I include \
   -I "$CJSON_DIR" \
   test/test_interactive_session.c \
   src/interactive_session.c \
   src/frame_codec.c \
   src/deck.c \
   src/frame_collection.c \
   "$CJSON_DIR/cJSON.c" \
   -lm \
   -o "$OUT"
"$OUT"
rc=$?
rm -f "$OUT"
exit $rc
