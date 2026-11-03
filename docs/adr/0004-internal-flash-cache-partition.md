# 0004: Two-Tier SD-Free Caching (PSRAM Neighbor Prefetch + Internal Flash Cache)

Alongside optional microSD storage, the E1003 uses two internal cache tiers: (1) volatile PSRAM Neighbor Prefetch of adjacent deck pages during an Interactive Session, and (2) a persistent internal Flash Cache partition starting after `0x830000` (sized after confirming physical flash capacity via `esptool flash_id`).

## Consequences

- Enables instant deck navigation and offline persistence of Base Frames and Widget Region data with zero microSD card installed, and avoids the 4 MHz shared SPI SD bus bottleneck (~7.3 s per cached page turn in baseline benchmarks).
- Adopting the expanded partition table requires a one-time USB flash (preserving `nvs` at `0x9000` so Wi-Fi credentials and server pairing survive); subsequent firmware updates continue to use A/B OTA.
