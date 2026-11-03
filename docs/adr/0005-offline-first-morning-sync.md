# 0005: Offline-First Scheduled Sync with Radio-Off Interactive Wakes

Rather than starting Wi-Fi on human button or touch wakes, the E1003 operates radio-off by default: an RTC-timed Scheduled Sync (e.g. each morning) turns on Wi-Fi once to download a complete Sync Bundle (compressed Base Frames, Widget Region specs, and multi-day calendar/weather JSON) into the Flash Cache and powers Wi-Fi off. All daytime Interactive Sessions wake directly into Flash Cache + PSRAM with Wi-Fi disabled, while an in-app Overlay Modal provides an explicit Ad-Hoc Sync ("Sync Now" / live Wi-Fi toggle) when fresh data is wanted immediately.

## Consequences

- Cuts cold-wake latency from ~13.9 s (WPA2 + DHCP + HTTP + GC16) to ~500 ms (25 ms flash decompress + 435 ms Mode 5 Fast Pass) and eliminates ~300 mA Wi-Fi current spikes across daytime interactions.
- External feeds (calendar, weather, gallery rotation) reflect the last Scheduled or Ad-Hoc Sync rather than live sub-minute server pushes unless the user opens a live Wi-Fi session from the Overlay Modal.
