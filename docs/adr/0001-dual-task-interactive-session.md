# 0001: Dual-Task Interactive Session with Warm Display Standby

On button or touch wake, the E1003 enters a unified FreeRTOS dual-task Interactive Session (~30 s idle timeout, reset on each input) rather than running separate blocking `touch_linger_s` and `button_wake_s` loops on `main_task`. During an Interactive Session, the IT8951 rails remain powered in `STANDBY` (`0x0002`), an input/UI task polls GT911 and GPIO buttons every 10–15 ms, and a dedicated Display Worker task drives SPI pixel transfers and `LUTAFSR` waits asynchronously; rails are powered down and latched low (`BOARD_EPD_HOLD_RAILS_IN_SLEEP`) only when the session expires into deep sleep.

## Consequences

- Eliminates cold IT8951 re-initialization, `s_reference_stale` white `INIT` flashes, and dropped swipes/taps during multi-second network or panel operations.
- Increases awake current slightly while the 30 s session is active, traded deliberately for sub-second interactive responsiveness.
