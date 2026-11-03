# 0002: Flashless Mode 5 Fast Pass with Idle GC16 Settle Pass

Interactive navigation and localised Widget Region updates on the E1003 use IT8951 waveform mode 5 (`EPD_IT8951_GRAY_MODE`, ~435 ms 16-level grayscale, flashless) as a Fast Pass, followed automatically by a `MODE_GC16` Settle Pass after ~2.5 s of user stillness (and before deep sleep if unsettled), removing full-screen `MODE_DU` invert echoes on page swipes and warm-session `MODE_INIT` white clears.

## Considered Options

- **Mode 6 (`A2`, ~220 ms)**: Rejected because hardware benching on the ED103TC2 showed severe ghosting on grayscale content.
- **Full `MODE_GC16` (~1564 ms) + `DU` echo + `INIT` clear on every interaction**: Rejected after simulator baseline measured 4.5 s of panel refresh time (3 passes) per swipe.
