#!/bin/sh
# Host-side unified cache_storage + compressed deck_cache / collection_cache
# behavioral tests over a temporary POSIX directory.
set -eu

PIO_CORE_DIR="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}"
CJSON_DIR="$PIO_CORE_DIR/packages/framework-espidf/components/json/cJSON"
if [ ! -f "$CJSON_DIR/cJSON.c" ]; then
    echo "ESP-IDF cJSON source not found at $CJSON_DIR" >&2
    exit 1
fi

OUT="$(mktemp -t tesserae-flash-cache.XXXXXX)"
cc -std=c11 -O2 -Wall -Wextra -Werror \
   -DTESSERAE_BOARD_SEEED_E1003 \
   -DTESSERAE_FLASH_CACHE=1 \
   -I src \
   -I include \
   -I boards \
   -I "$CJSON_DIR" \
   test/test_flash_cache.c \
   src/cache_storage.c \
   src/frame_codec.c \
   src/deck_cache.c \
   src/collection_cache.c \
   src/deck.c \
   src/frame_collection.c \
   "$CJSON_DIR/cJSON.c" \
   -lm \
   -o "$OUT"
"$OUT"
rc=$?
rm -f "$OUT"
exit $rc
