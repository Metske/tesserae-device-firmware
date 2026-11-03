# 0003: Vendor FreeInkUI C++17 Component Behind an `extern "C"` Seam

We vendor `FreeInkUI` and `FreeInkFont` (MIT, freestanding C++17) into `Metske/tesserae-device-firmware` as an ESP-IDF component paired with a custom 4bpp `DrawTarget` (`FreeInkTarget4bpp`) that renders anti-aliased 16-gray UI directly into PSRAM framebuffers (`0x0` black .. `0xF` white) and exposes a narrow `extern "C"` API to the existing C firmware.

## Considered Options

- **Port E1003 into CrossInk/FreeInk SDK**: Rejected because FreeInk's hardware/display/SD layers depend on Arduino (`SPIClass`, `SdFat`) and M5Paper 1bpp+overlay assumptions, whereas Tesserae's ESP-IDF `it8951_gray.c`, OTA, BLE provisioning, and REST/SSE stack are already proven on E1003.
- **Extend `touch3` C primitives by hand**: Rejected because `touch3` only draws 4 fixed control shapes (`button`, `switch`, `slider`, `stepper`), whereas `FreeInkUI` already provides layout containers, lists, tabs, keyboards, overlays, and host tests.
