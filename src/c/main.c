#include <pebble.h>

#define PERSIST_BG_COLOR      1
#define PERSIST_DIAL_COLOR    2
#define PERSIST_HANDS_COLOR   3
#define PERSIST_TRANSPARENT   4
#define PERSIST_SECOND_HAND   5
#define PERSIST_ACTIVITY      6
#define PERSIST_TRACKER_FONT  7

#define TAP_DEBOUNCE_SECONDS  2

typedef enum {
    ACT_STEPS = 0,
    ACT_ACTIVE_MINUTES,
    ACT_CALORIES,
    ACT_DISTANCE,
    ACT_HEART_RATE,
    ACT_BATTERY,
    ACT_DIGITAL_TIME,
    NUM_ACTIVITIES
} ActivityType;

static const uint32_t ICON_RES_IDS[] = {
    RESOURCE_ID_ICON_STEPS,
    RESOURCE_ID_ICON_ACTIVE_MINUTES,
    RESOURCE_ID_ICON_CALORIES,
    RESOURCE_ID_ICON_DISTANCE,
    RESOURCE_ID_ICON_HEART_RATE,
    RESOURCE_ID_ICON_BATTERY,
    RESOURCE_ID_ICON_DIGITAL_TIME,
};

static GColor s_bg_color;
static GColor s_dial_color;
static GColor s_hands_color;
static bool s_transparent_hands;
static bool s_second_hand_enabled;
static ActivityType s_activity;
static bool s_large_font;

static int s_hours, s_minutes, s_seconds;
static int s_heart_rate;
static int s_battery_charge;
static char s_tracker_buf[16];
static char s_date_buf[4];
static time_t s_last_tap_time;

static Window *s_window;
static Layer *s_bg_layer;
static Layer *s_hands_layer;
static Layer *s_date_border_layer;
static TextLayer *s_tracker_text_layer;
static TextLayer *s_date_text_layer;
static BitmapLayer *s_tracker_icon_layer;

static GBitmap *s_icons[NUM_ACTIVITIES];
static GFont s_font_dial;
static GFont s_font_date;


static void tick_handler(struct tm *tick_time, TimeUnits units_changed);

// ----- helpers -----

static int health_sum_today(HealthMetric metric) {
    HealthServiceAccessibilityMask mask =
        health_service_metric_accessible(metric, time_start_of_today(), time(NULL));
    if (mask & HealthServiceAccessibilityMaskAvailable) {
        return (int)health_service_sum_today(metric);
    }
    return 0;
}

static void update_activity_display(void) {
    switch (s_activity) {
        case ACT_STEPS:
            snprintf(s_tracker_buf, sizeof(s_tracker_buf), "%d",
                     health_sum_today(HealthMetricStepCount));
            break;
        case ACT_ACTIVE_MINUTES: {
            int sec = health_sum_today(HealthMetricActiveSeconds);
            snprintf(s_tracker_buf, sizeof(s_tracker_buf), "%d:%02d",
                     sec / 3600, (sec % 3600) / 60);
            break;
        }
        case ACT_CALORIES: {
            int total = health_sum_today(HealthMetricActiveKCalories) +
                        health_sum_today(HealthMetricRestingKCalories);
            snprintf(s_tracker_buf, sizeof(s_tracker_buf), "%d", total);
            break;
        }
        case ACT_DISTANCE: {
            int m = health_sum_today(HealthMetricWalkedDistanceMeters);
            snprintf(s_tracker_buf, sizeof(s_tracker_buf), "%d.%d km",
                     m / 1000, (m % 1000) / 100);
            break;
        }
        case ACT_HEART_RATE: {
            HealthServiceAccessibilityMask hr_mask =
                health_service_metric_accessible(HealthMetricHeartRateBPM,
                                                  time(NULL) - 120, time(NULL));
            if (hr_mask & HealthServiceAccessibilityMaskAvailable) {
                s_heart_rate = (int)health_service_peek_current_value(HealthMetricHeartRateBPM);
            }
            if (s_heart_rate > 0) {
                snprintf(s_tracker_buf, sizeof(s_tracker_buf), "%d", s_heart_rate);
            } else {
                snprintf(s_tracker_buf, sizeof(s_tracker_buf), "...");
            }
            break;
        }
        case ACT_BATTERY:
            snprintf(s_tracker_buf, sizeof(s_tracker_buf), "%d%%", s_battery_charge);
            break;
        case ACT_DIGITAL_TIME: {
            time_t now = time(NULL);
            struct tm *t = localtime(&now);
            if (clock_is_24h_style()) {
                strftime(s_tracker_buf, sizeof(s_tracker_buf), "%H:%M", t);
            } else {
                strftime(s_tracker_buf, sizeof(s_tracker_buf), "%l:%M %p", t);
            }
            break;
        }
        default:
            s_tracker_buf[0] = '\0';
            break;
    }
    text_layer_set_text(s_tracker_text_layer, s_tracker_buf);
}

static void update_icon_display(void) {
    bitmap_layer_set_bitmap(s_tracker_icon_layer, s_icons[s_activity]);
}

static void apply_colors(void) {
    window_set_background_color(s_window, s_bg_color);
    text_layer_set_text_color(s_tracker_text_layer, s_dial_color);
    text_layer_set_text_color(s_date_text_layer, s_dial_color);
    layer_mark_dirty(s_bg_layer);
    layer_mark_dirty(s_hands_layer);
    layer_mark_dirty(s_date_border_layer);
}

static void apply_tracker_font(void) {
    text_layer_set_font(s_tracker_text_layer,
        fonts_get_system_font(s_large_font ?
            FONT_KEY_GOTHIC_28_BOLD : FONT_KEY_GOTHIC_24_BOLD));
}

// ----- settings persistence -----

static void load_settings(void) {
    s_bg_color = persist_exists(PERSIST_BG_COLOR) ?
        (GColor){ .argb = (uint8_t)persist_read_int(PERSIST_BG_COLOR) } : GColorBlack;
    s_dial_color = persist_exists(PERSIST_DIAL_COLOR) ?
        (GColor){ .argb = (uint8_t)persist_read_int(PERSIST_DIAL_COLOR) } : GColorWhite;
    s_hands_color = persist_exists(PERSIST_HANDS_COLOR) ?
        (GColor){ .argb = (uint8_t)persist_read_int(PERSIST_HANDS_COLOR) } : GColorRed;
    s_transparent_hands = persist_exists(PERSIST_TRANSPARENT) ?
        (persist_read_int(PERSIST_TRANSPARENT) != 0) : false;
    s_second_hand_enabled = persist_exists(PERSIST_SECOND_HAND) ?
        (persist_read_int(PERSIST_SECOND_HAND) != 0) : true;
    s_activity = persist_exists(PERSIST_ACTIVITY) ?
        (ActivityType)persist_read_int(PERSIST_ACTIVITY) : ACT_STEPS;
    s_large_font = persist_exists(PERSIST_TRACKER_FONT) ?
        (persist_read_int(PERSIST_TRACKER_FONT) != 0) : false;
    if (s_activity >= NUM_ACTIVITIES) s_activity = ACT_STEPS;
}

// ----- drawing -----

static void bg_update_proc(Layer *layer, GContext *ctx) {
    GRect bounds = layer_get_bounds(layer);
    static const char *nums[] = {
        "12", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11"
    };
    // Center positions scaled from the original 336x336 background
    // The layout is a rounded rectangle, not a circle or grid
    static const int16_t centers[12][2] = {
        {100,  18},  // 12: top center
        {162,  18},  // 1:  top right corner
        {183,  63},  // 2:  right upper edge
        {183, 112},  // 3:  right center edge
        {183, 161},  // 4:  right lower edge
        {158, 210},  // 5:  bottom right corner
        {100, 210},  // 6:  bottom center
        { 40, 210},  // 7:  bottom left corner
        { 18, 161},  // 8:  left lower edge
        { 18, 112},  // 9:  left center edge
        { 18,  63},  // 10: left upper edge
        { 40,  18},  // 11: top left corner
    };

    graphics_context_set_text_color(ctx, s_dial_color);
    for (int i = 0; i < 12; i++) {
        GRect box = GRect(centers[i][0] - 25, centers[i][1] - 18, 50, 36);
        graphics_draw_text(ctx, nums[i], s_font_dial, box,
                           GTextOverflowModeTrailingEllipsis,
                           GTextAlignmentCenter, NULL);
    }
}

static void rotate_point(GPoint center, int px, int py, int32_t angle, GPoint *out) {
    int32_t sa = sin_lookup(angle);
    int32_t ca = cos_lookup(angle);
    int d = -py;
    out->x = center.x + (sa * d + ca * px) / TRIG_MAX_RATIO;
    out->y = center.y + (-ca * d + sa * px) / TRIG_MAX_RATIO;
}

static void draw_opaque_hand(GContext *ctx, GPoint center,
                              int32_t angle, int tip_d, int base_d, int width) {
    GPoint tip, base;
    rotate_point(center, 0, tip_d, angle, &tip);
    rotate_point(center, 0, base_d, angle, &base);

    graphics_context_set_stroke_color(ctx, s_hands_color);
    graphics_context_set_stroke_width(ctx, width);
    graphics_draw_line(ctx, tip, base);

    graphics_context_set_fill_color(ctx, s_hands_color);
    graphics_fill_circle(ctx, tip, width / 2);
    graphics_fill_circle(ctx, base, width / 2);
}

static void draw_transparent_hand(GContext *ctx, GPoint center,
                                   int32_t angle, int half_w,
                                   int line_top, int line_bot) {
    GPoint lt, lb, rt, rb;
    rotate_point(center, -half_w, line_top, angle, &lt);
    rotate_point(center, -half_w, line_bot, angle, &lb);
    rotate_point(center,  half_w, line_top, angle, &rt);
    rotate_point(center,  half_w, line_bot, angle, &rb);

    graphics_context_set_stroke_color(ctx, s_hands_color);
    graphics_context_set_stroke_width(ctx, 2);
    graphics_draw_line(ctx, lt, lb);
    graphics_draw_line(ctx, rt, rb);

    GPoint top_c, bot_c;
    rotate_point(center, 0, line_top, angle, &top_c);
    rotate_point(center, 0, line_bot, angle, &bot_c);

    int arc_d = half_w * 2 + 1;
    int32_t start_out = angle - TRIG_MAX_ANGLE / 4;
    int32_t end_out   = angle + TRIG_MAX_ANGLE / 4;
    graphics_draw_arc(ctx,
        GRect(top_c.x - half_w, top_c.y - half_w, arc_d, arc_d),
        GOvalScaleModeFitCircle, start_out, end_out);

    int32_t start_in = angle + TRIG_MAX_ANGLE / 4;
    int32_t end_in   = angle + 3 * TRIG_MAX_ANGLE / 4;
    graphics_draw_arc(ctx,
        GRect(bot_c.x - half_w, bot_c.y - half_w, arc_d, arc_d),
        GOvalScaleModeFitCircle, start_in, end_in);
}

static void draw_root_line(GContext *ctx, GPoint center, int32_t angle,
                            int length, GColor color) {
    GPoint top, bot;
    rotate_point(center, 0, -length, angle, &top);
    bot = center;
    graphics_context_set_stroke_color(ctx, color);
    graphics_context_set_stroke_width(ctx, 3);
    graphics_draw_line(ctx, top, bot);
}

static void hands_update_proc(Layer *layer, GContext *ctx) {
    GRect bounds = layer_get_bounds(layer);
    GPoint center = GPoint(bounds.size.w / 2, bounds.size.h / 2);

    int32_t hour_total = (s_hours % 12) * 3600 + s_minutes * 60 + s_seconds;
    int32_t hour_angle = (int32_t)((int64_t)TRIG_MAX_ANGLE * hour_total / 43200);
    int32_t min_total = s_minutes * 60 + s_seconds;
    int32_t min_angle = (int32_t)((int64_t)TRIG_MAX_ANGLE * min_total / 3600);

    if (s_transparent_hands) {
        draw_transparent_hand(ctx, center, hour_angle, 4, -54, -18);
        draw_transparent_hand(ctx, center, min_angle,  4, -77, -18);
    } else {
        draw_opaque_hand(ctx, center, hour_angle, -58, -14, 8);
        draw_opaque_hand(ctx, center, min_angle, -81, -14, 8);
    }

    draw_root_line(ctx, center, hour_angle, 14, s_hands_color);
    draw_root_line(ctx, center, min_angle,  14, s_hands_color);

    if (s_second_hand_enabled) {
        int32_t sec_angle = TRIG_MAX_ANGLE * s_seconds / 60;

        GPoint sec_tip, sec_tail;
        rotate_point(center, 0, -77, sec_angle, &sec_tip);
        rotate_point(center, 0,  14, sec_angle, &sec_tail);

        graphics_context_set_stroke_color(ctx, s_dial_color);
        graphics_context_set_stroke_width(ctx, 1);
        graphics_draw_line(ctx, sec_tip, sec_tail);
    }

    graphics_context_set_fill_color(ctx, s_hands_color);
    graphics_fill_circle(ctx, center, 5);
    graphics_context_set_fill_color(ctx, s_dial_color);
    graphics_fill_circle(ctx, center, 4);
    graphics_context_set_fill_color(ctx, s_bg_color);
    graphics_fill_circle(ctx, center, 2);
}

static void date_border_update_proc(Layer *layer, GContext *ctx) {
    GRect b = layer_get_bounds(layer);
    graphics_context_set_stroke_color(ctx, s_dial_color);
    graphics_context_set_stroke_width(ctx, 1);
    graphics_draw_rect(ctx, b);
}

// ----- tick service -----

static void update_tick_subscription(void) {
    tick_timer_service_unsubscribe();
    tick_timer_service_subscribe(
        s_second_hand_enabled ? SECOND_UNIT : MINUTE_UNIT, tick_handler);
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
    s_hours = tick_time->tm_hour;
    s_minutes = tick_time->tm_min;
    s_seconds = tick_time->tm_sec;

    if (units_changed & DAY_UNIT) {
        snprintf(s_date_buf, sizeof(s_date_buf), "%02d", tick_time->tm_mday);
        text_layer_set_text(s_date_text_layer, s_date_buf);
    }

    if (units_changed & MINUTE_UNIT) {
        update_activity_display();
    }

    layer_mark_dirty(s_hands_layer);
}

// ----- tap handler -----

static void tap_handler(AccelAxisType axis, int32_t direction) {
    (void)axis;
    (void)direction;
    time_t now = time(NULL);
    if (now - s_last_tap_time < TAP_DEBOUNCE_SECONDS) return;
    s_last_tap_time = now;

    s_activity = (s_activity + 1) % NUM_ACTIVITIES;
    persist_write_int(PERSIST_ACTIVITY, s_activity);
    update_activity_display();
    update_icon_display();
}

// ----- health & battery -----

static void health_handler(HealthEventType event, void *context) {
    if (event == HealthEventHeartRateUpdate) {
        HealthServiceAccessibilityMask mask =
            health_service_metric_accessible(HealthMetricHeartRateBPM,
                                              time(NULL) - 120, time(NULL));
        if (mask & HealthServiceAccessibilityMaskAvailable) {
            s_heart_rate = (int)health_service_peek_current_value(HealthMetricHeartRateBPM);
        }
    }
    if (s_activity == ACT_HEART_RATE || s_activity == ACT_STEPS ||
        s_activity == ACT_ACTIVE_MINUTES || s_activity == ACT_CALORIES ||
        s_activity == ACT_DISTANCE) {
        update_activity_display();
    }
}

static void battery_handler(BatteryChargeState charge) {
    s_battery_charge = charge.charge_percent;
    if (s_activity == ACT_BATTERY) {
        update_activity_display();
    }
}

// ----- AppMessage -----

static void inbox_received(DictionaryIterator *iter, void *context) {
    Tuple *t;
    bool need_tick_update = false;

    t = dict_find(iter, MESSAGE_KEY_BACKGROUND_COLOR);
    if (t) {
        s_bg_color = GColorFromHEX(t->value->int32);
        persist_write_int(PERSIST_BG_COLOR, s_bg_color.argb);
    }

    t = dict_find(iter, MESSAGE_KEY_DIAL_COLOR);
    if (t) {
        s_dial_color = GColorFromHEX(t->value->int32);
        persist_write_int(PERSIST_DIAL_COLOR, s_dial_color.argb);
    }

    t = dict_find(iter, MESSAGE_KEY_HANDS_COLOR);
    if (t) {
        s_hands_color = GColorFromHEX(t->value->int32);
        persist_write_int(PERSIST_HANDS_COLOR, s_hands_color.argb);
    }

    t = dict_find(iter, MESSAGE_KEY_TRANSPARENT_HANDS);
    if (t) {
        s_transparent_hands = (t->value->int32 != 0);
        persist_write_int(PERSIST_TRANSPARENT, s_transparent_hands ? 1 : 0);
    }

    t = dict_find(iter, MESSAGE_KEY_SECOND_HAND);
    if (t) {
        bool new_val = (t->value->int32 != 0);
        if (new_val != s_second_hand_enabled) {
            s_second_hand_enabled = new_val;
            persist_write_int(PERSIST_SECOND_HAND, s_second_hand_enabled ? 1 : 0);
            need_tick_update = true;
        }
    }

    t = dict_find(iter, MESSAGE_KEY_ACTIVITY);
    if (t) {
        int v = atoi(t->value->cstring);
        if (v >= 0 && v < NUM_ACTIVITIES) {
            s_activity = (ActivityType)v;
            persist_write_int(PERSIST_ACTIVITY, v);
            update_icon_display();
        }
    }

    t = dict_find(iter, MESSAGE_KEY_TRACKER_FONT_SIZE);
    if (t) {
        s_large_font = (atoi(t->value->cstring) == 1);
        persist_write_int(PERSIST_TRACKER_FONT, s_large_font ? 1 : 0);
        apply_tracker_font();
    }

    apply_colors();
    update_activity_display();

    if (need_tick_update) {
        update_tick_subscription();
    }
}

static void inbox_dropped(AppMessageResult reason, void *context) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "Message dropped: %d", (int)reason);
}

// ----- window -----

static void window_load(Window *window) {
    Layer *window_layer = window_get_root_layer(window);
    GRect bounds = layer_get_bounds(window_layer);
    int cx = bounds.size.w / 2;
    int cy = bounds.size.h / 2;

    s_bg_layer = layer_create(bounds);
    layer_set_update_proc(s_bg_layer, bg_update_proc);
    layer_add_child(window_layer, s_bg_layer);

    s_tracker_text_layer = text_layer_create(GRect(0, cy - 68, bounds.size.w, 32));
    text_layer_set_background_color(s_tracker_text_layer, GColorClear);
    text_layer_set_text_color(s_tracker_text_layer, s_dial_color);
    text_layer_set_text_alignment(s_tracker_text_layer, GTextAlignmentCenter);
    apply_tracker_font();
    layer_add_child(window_layer, text_layer_get_layer(s_tracker_text_layer));

    s_tracker_icon_layer = bitmap_layer_create(GRect(cx - 10, cy - 40, 20, 20));
    bitmap_layer_set_compositing_mode(s_tracker_icon_layer, GCompOpSet);
    layer_add_child(window_layer, bitmap_layer_get_layer(s_tracker_icon_layer));

    int date_w = 36;
    int date_h = 24;
    int date_y = cy + 48;
    s_date_border_layer = layer_create(GRect(cx - date_w / 2 - 3, date_y - 2,
                                              date_w + 6, date_h + 4));
    layer_set_update_proc(s_date_border_layer, date_border_update_proc);
    layer_add_child(window_layer, s_date_border_layer);

    s_date_text_layer = text_layer_create(GRect(cx - date_w / 2, date_y, date_w, date_h));
    text_layer_set_background_color(s_date_text_layer, GColorClear);
    text_layer_set_text_color(s_date_text_layer, s_dial_color);
    text_layer_set_font(s_date_text_layer, s_font_date);
    text_layer_set_text_alignment(s_date_text_layer, GTextAlignmentCenter);
    layer_add_child(window_layer, text_layer_get_layer(s_date_text_layer));

    s_hands_layer = layer_create(bounds);
    layer_set_update_proc(s_hands_layer, hands_update_proc);
    layer_add_child(window_layer, s_hands_layer);

    for (int i = 0; i < NUM_ACTIVITIES; i++) {
        s_icons[i] = gbitmap_create_with_resource(ICON_RES_IDS[i]);
    }

    update_icon_display();
    update_activity_display();

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    s_hours = t->tm_hour;
    s_minutes = t->tm_min;
    s_seconds = t->tm_sec;
    snprintf(s_date_buf, sizeof(s_date_buf), "%02d", t->tm_mday);
    text_layer_set_text(s_date_text_layer, s_date_buf);

    apply_colors();
}

static void window_unload(Window *window) {
    layer_destroy(s_bg_layer);
    layer_destroy(s_hands_layer);
    layer_destroy(s_date_border_layer);
    text_layer_destroy(s_tracker_text_layer);
    text_layer_destroy(s_date_text_layer);
    bitmap_layer_destroy(s_tracker_icon_layer);

    for (int i = 0; i < NUM_ACTIVITIES; i++) {
        gbitmap_destroy(s_icons[i]);
    }
}

// ----- init / deinit -----

static void init(void) {
    s_font_dial = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_ZENDOTS_26));
    s_font_date = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_ZENDOTS_18));

    load_settings();

    s_window = window_create();
    window_set_background_color(s_window, s_bg_color);
    window_set_window_handlers(s_window, (WindowHandlers) {
        .load = window_load,
        .unload = window_unload,
    });
    window_stack_push(s_window, true);

    update_tick_subscription();
    accel_tap_service_subscribe(tap_handler);

    health_service_events_subscribe(health_handler, NULL);
    s_battery_charge = battery_state_service_peek().charge_percent;
    battery_state_service_subscribe(battery_handler);

    app_message_register_inbox_received(inbox_received);
    app_message_register_inbox_dropped(inbox_dropped);
    app_message_open(256, 64);
}

static void deinit(void) {
    tick_timer_service_unsubscribe();
    accel_tap_service_unsubscribe();
    health_service_events_unsubscribe();
    battery_state_service_unsubscribe();

    fonts_unload_custom_font(s_font_dial);
    fonts_unload_custom_font(s_font_date);

    window_destroy(s_window);
}

int main(void) {
    init();
    app_event_loop();
    deinit();
}
