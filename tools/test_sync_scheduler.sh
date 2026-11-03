#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

OUT="/tmp/test_sync_scheduler_bin"

cc -std=c11 -Wall -Wextra -Werror \
    -Isrc \
    src/sync_scheduler.c \
    test/test_sync_scheduler.c \
    -o "$OUT"

"$OUT"
