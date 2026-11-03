# 0007: On-Demand Radio Burst Controls, Passive RTC Refresh, and Input Routing

During radio-off Interactive Sessions, local controls execute offline while network-bound controls (such as Home Assistant actions or webhooks) trigger a temporary Radio Burst with a localised "Connecting..." indicator and tear Wi-Fi back down when the session ends. Daily Scheduled Sync defaults to 06:00 local (auto-syncing on empty-cache cold boot, with up to two +30 min retries before falling back to cached multi-day data), configurable radio-off Passive Refresh wakes advance gallery/deck art and clock/sensor regions from Flash Cache, and input routes Widget Region touches to widgets, horizontal swipes or Left/Right buttons (GPIO5/4) to prev/next frames, and top-edge swipe down or short-press middle button (GPIO3) to the FreeInkUI Overlay Modal (preserving >4 s GPIO3 hold for BLE maintenance).

## Consequences

- Keeps normal daytime navigation, widget scrolling, and scheduled gallery/clock ticks 100% radio-off while still letting Home Assistant and webhook controls work seamlessly with a single tap.
- Repurposes short-press GPIO3 from an immediate full refresh into opening/closing the Overlay Modal (which exposes Clean GC16 Refresh, Deck/Gallery Switcher, Sync Now, and Live Wi-Fi Toggle inside one menu).
