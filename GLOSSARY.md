# Tesserae Device Firmware (FreeInkUI Edition)

On-device firmware for e-paper panels paired with a Tesserae companion server, combining server-composed frames with responsive local UI rendering and offline-capable caching.

## Display & Composition

**Base Frame**:
The full-panel static image composed by the Tesserae server onto which dynamic on-device UI regions or overlays may be composited.
_Avoid_: Background bitmap, wallpaper, raw frame

**Widget Region**:
A bounded rectangular area within a Base Frame that is owned, laid out, and redrawn locally on the device by FreeInkUI using localised partial refreshes without disturbing surrounding pixels.
_Avoid_: Slot, touch3 box, sub-screen, patch rect

**Overlay Modal**:
A transient on-device FreeInkUI surface (such as a launcher drawer, quick switcher, or settings sheet) drawn temporarily over part or all of the current Base Frame and dismissed back to the underlying Base Frame.
_Avoid_: Popup frame, HUD, chrome page

## Interaction & Refresh

**Interactive Session**:
The active period triggered by a touch or button wake during which the display controller stays warm and user input is continuously serviced until an inactivity timeout returns the device to deep sleep.
_Avoid_: Linger window, button wake window, awake loop

**Fast Pass**:
A low-latency, flashless grayscale update used during active navigation or widget interaction to provide immediate visual response.
_Avoid_: Draft repaint, quick flash, echo pass

**Settle Pass**:
A full-fidelity, ghost-clearing grayscale update applied automatically after interaction pauses (or before deep sleep) so the resting panel shows clean, artifact-free tones.
_Avoid_: Hygiene pass, cleanup flash, slow refresh

## Caching & Synchronisation

**Sync Bundle**:
The self-contained package of compressed Base Frames, deck manifests, Widget Region specifications, and structured datasets (such as multi-day calendar agendas and hourly weather forecasts) pulled from the server in one pass for offline operation.
_Avoid_: Cache dump, daily payload, offline zip

**Scheduled Sync**:
An unattended, RTC-timed radio wake (such as once each morning) that downloads a fresh Sync Bundle into the Flash Cache, disciplines the hardware clock, reports device telemetry, and powers Wi-Fi back off.
_Avoid_: Heartbeat, timer poll, cron wake

**Ad-Hoc Sync**:
A user-initiated Wi-Fi connection triggered from the Overlay Modal to pull an updated Sync Bundle or exchange live state on demand.
_Avoid_: Manual poll, force fetch

**Radio Burst**:
A temporary Wi-Fi connection opened automatically during a radio-off Interactive Session when the user invokes a network-bound control, then powered back off when the session ends.
_Avoid_: Background reconnect, lazy Wi-Fi

**Passive Refresh**:
A brief, radio-off RTC timer wake that advances cached gallery or deck frames or updates localised clock, sensor, and hourly weather regions directly from the Flash Cache before returning to deep sleep.
_Avoid_: Silent poll, offline heartbeat

**Neighbor Prefetch**:
Pre-loading adjacent deck pages into volatile memory during an Interactive Session so forward and backward navigation paints immediately without waiting on storage or network transfers.
_Avoid_: Prewarm, lookahead buffer

**Flash Cache**:
Persistent on-board flash storage that retains Sync Bundles across deep sleep even when no removable card is installed.
_Avoid_: Internal SD, ROM cache, NVS cache
