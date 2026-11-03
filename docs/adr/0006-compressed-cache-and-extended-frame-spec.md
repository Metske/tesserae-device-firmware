# 0006: Compressed Frame Storage and `/frame/spec` `widgets[]` Extension

Base Frames in the Flash Cache and PSRAM Neighbor Prefetch are stored compressed (LZ4/deflate, decoded into `s_base`/`s_work` in <25 ms on ESP32-S3), and dynamic FreeInkUI Widget Regions are declared by extending `Metske/tesserae`'s existing `/frame/spec` and `/frame/data` contract with a `widgets[]` block rather than inventing a parallel REST surface.

## Consequences

- Increases capacity from ~2 to ~8+ frames in 8 MB PSRAM and from ~17 to ~50–100+ frames in a 32 MB Flash Cache while cutting Morning/Ad-Hoc Sync download time by ~4x.
- Reuses Tesserae's existing coordinate/underscan transformation, digest invalidation, and values envelope plumbing on the server.
