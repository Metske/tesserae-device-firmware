/*
 * widget_region.h -- FreeInkUI embedded Widget Regions & status_strip
 * hybrid composition for Tesserae (ADR-0003, Ticket #6 / T5).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIDGET_MAX_COUNT           8
#define WIDGET_HOURLY_MAX         24
#define WIDGET_EVENT_MAX          32
#define WIDGET_STR_CAP            32
#define WIDGET_EVENT_STR_CAP      64

typedef enum {
    WIDGET_KIND_UNKNOWN = 0,
    WIDGET_KIND_STATUS_STRIP,
    WIDGET_KIND_AGENDA_LIST,
    WIDGET_KIND_CONTROL_GROUP,
} widget_kind_t;

typedef enum {
    WIDGET_TOUCH_NONE = 0,
    WIDGET_TOUCH_HANDLED_DIRTY,   /* Handled internally; work_fb re-rendered, out_dirty set */
    WIDGET_TOUCH_NETWORK_BURST,   /* Network control tapped, optimistic state drawn, burst requested */
    WIDGET_TOUCH_UNCLAIMED,       /* Touch fell outside or wasn't handled by any widget */
} widget_touch_result_t;

typedef enum {
    WIDGET_CTRL_UNKNOWN = 0,
    WIDGET_CTRL_BUTTON,
    WIDGET_CTRL_SWITCH,
    WIDGET_CTRL_SLIDER,
    WIDGET_CTRL_STEPPER,
    WIDGET_CTRL_TABS,
} widget_control_type_t;

typedef struct {
    char                  id[WIDGET_STR_CAP];
    widget_control_type_t type;
    char                  label[WIDGET_STR_CAP];
    char                  action[WIDGET_STR_CAP];
    char                  value_key[WIDGET_STR_CAP];
    bool                  network;
    bool                  state;
    float                 value;
    float                 min;
    float                 max;
    float                 step;
    int                   tabs_count;
    char                  tabs_labels[4][WIDGET_STR_CAP];
    int                   tabs_selected;
    char                  status_badge[16];
} widget_control_t;

typedef struct {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
} widget_rect_t;

typedef struct {
    int64_t epoch;
    float   temp_c;
    char    condition[WIDGET_STR_CAP];
    char    icon[WIDGET_STR_CAP];
} widget_hourly_forecast_t;

typedef struct {
    char    id[WIDGET_STR_CAP];
    char    title[WIDGET_EVENT_STR_CAP];
    int64_t start_epoch;
    int64_t end_epoch;
    char    date_str[WIDGET_STR_CAP];
    char    time_str[WIDGET_STR_CAP];
    char    location[WIDGET_STR_CAP];
    bool    all_day;
} widget_calendar_event_t;

typedef struct {
    char          id[WIDGET_STR_CAP];
    widget_kind_t kind;
    widget_rect_t rect;          /* Raw panel-space rect from spec */
    widget_rect_t aligned_rect;  /* 4-pixel horizontally aligned rect */
    bool          bind_clock;
    bool          bind_env;
    bool          bind_battery;
    bool          bind_weather;
    bool          bind_calendar;
    int           agenda_page_index;
    widget_control_t controls[8];
    size_t        control_count;
} widget_descriptor_t;

typedef struct {
    widget_descriptor_t     widgets[WIDGET_MAX_COUNT];
    size_t                  widget_count;
    widget_hourly_forecast_t hourly[WIDGET_HOURLY_MAX];
    size_t                  hourly_count;
    widget_calendar_event_t events[WIDGET_EVENT_MAX];
    size_t                  event_count;
} widget_spec_t;

typedef struct {
    int64_t now_epoch;
    int32_t tz_offset_s;
    float   temperature_c;
    float   humidity_pct;
    bool    has_sht4x;
    int     battery_pct;
} widget_env_state_t;

/* Parse widgets[] from frame spec JSON or bundle JSON.
 * Clamps/skips invalid/out-of-bounds rects (panel dims native_w x native_h).
 */
bool widget_spec_parse_json(const char *json_str,
                            int native_w,
                            int native_h,
                            widget_spec_t *out_spec);

/* Parse datasets.forecast.hourly from bundle JSON or frame data JSON. */
bool widget_spec_parse_datasets_json(const char *json_str,
                                     widget_spec_t *inout_spec);

/* Render all active widgets in spec onto work_fb over base_fb.
 * For status_strip: draws clock, climate, battery, hourly forecast.
 * Preserves all pixels outside the widget aligned_rects.
 * Returns true if any widget was painted.
 */
bool widget_region_render_all(const widget_spec_t *spec,
                              const widget_env_state_t *env,
                              const uint8_t *base_fb,
                              uint8_t *work_fb,
                              int panel_w,
                              int panel_h,
                              widget_rect_t *out_dirty_rect);

/* Render a single status_strip widget onto work_fb. */
bool widget_region_render_status_strip(const widget_descriptor_t *w,
                                       const widget_spec_t *spec,
                                       const widget_env_state_t *env,
                                       const uint8_t *base_fb,
                                       uint8_t *work_fb,
                                       int panel_w,
                                       int panel_h,
                                       widget_rect_t *out_dirty_rect);

/* Get active (future/current) events from spec, filtering out past events */
size_t widget_agenda_get_active_events(const widget_spec_t *spec,
                                       int64_t now_epoch,
                                       const widget_calendar_event_t **out_events,
                                       size_t max_out);

/* Render a single agenda_list widget onto work_fb. */
bool widget_region_render_agenda_list(widget_descriptor_t *w,
                                      const widget_spec_t *spec,
                                      const widget_env_state_t *env,
                                      const uint8_t *base_fb,
                                      uint8_t *work_fb,
                                      int panel_w,
                                      int panel_h,
                                      widget_rect_t *out_dirty_rect);

/* Render a single control_group widget onto work_fb. */
bool widget_region_render_control_group(widget_descriptor_t *w,
                                        const widget_spec_t *spec,
                                        const widget_env_state_t *env,
                                        const uint8_t *base_fb,
                                        uint8_t *work_fb,
                                        int panel_w,
                                        int panel_h,
                                        widget_rect_t *out_dirty_rect);

/* Reconcile a control with authoritative server state or offline revert */
void widget_control_reconcile(widget_control_t *ctrl,
                              bool success,
                              bool authoritative_state,
                              float authoritative_value);

/* Handle touch/gestures inside widget regions (e.g. vertical swipe/tap pagination). */
widget_touch_result_t widget_region_handle_touch(widget_spec_t *spec,
                                                 int x0, int y0,
                                                 int x1, int y1,
                                                 bool is_tap,
                                                 const widget_env_state_t *env,
                                                 const uint8_t *base_fb,
                                                 uint8_t *work_fb,
                                                 int panel_w,
                                                 int panel_h,
                                                 widget_rect_t *out_dirty);

/* Find matching hourly forecast slice for now_epoch */
const widget_hourly_forecast_t *widget_find_hourly_forecast(const widget_spec_t *spec,
                                                            int64_t now_epoch);

/* Global singleton widget spec for active frame & datasets */
widget_spec_t *widget_region_get_current_spec(void);

/* Check if current spec holds any active widgets */
bool widget_region_has_active_widgets(void);

/* Passive refresh wake: if active widgets exist, re-render dirty rect onto current
 * frame and emit a fast partial refresh without enabling radio.
 */
bool widget_region_try_passive_refresh(void);

#ifdef __cplusplus
}
#endif
