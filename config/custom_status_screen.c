/*
 *
 * Copyright (c) 2021 Darryl deHaan
 * SPDX-License-Identifier: MIT
 *
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/split/central.h>
#include <zmk/split/bluetooth/peripheral.h>

#include "widgets/output_status.h"
#include "widgets/layer_status.h"
#include "widgets/peripheral_status.h"
#include "widgets/battery_status.h"
#include "zen_minute_counter.h"
#include "zen_screen_layout.h"
#include "custom_status_screen.h"

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

LV_IMG_DECLARE(zenlogo);
LV_IMG_DECLARE(layers2);
static struct zmk_widget_battery_status battery_status_widget;
static lv_obj_t *status_layout_screen;
static lv_obj_t *status_layout_items[5];
static size_t status_layout_count;

static void status_layout_cb(struct k_work *work) {
    if (status_layout_screen == NULL) {
        return;
    }

    lv_obj_update_layout(status_layout_screen);
    int32_t heights[ARRAY_SIZE(status_layout_items)];
    int32_t tops[ARRAY_SIZE(status_layout_items)];
    for (size_t i = 0; i < status_layout_count; ++i) {
        heights[i] = lv_obj_get_height(status_layout_items[i]);
    }
    zen_screen_layout(lv_obj_get_content_height(status_layout_screen), heights, status_layout_count,
                      tops);

    for (size_t i = 0; i < status_layout_count; ++i) {
        lv_obj_t *item = status_layout_items[i];
        lv_coord_t centered_x = lv_obj_get_content_width(status_layout_screen) / 2 -
                                lv_obj_get_width(item) / 2;
        if (lv_obj_get_x(item) != centered_x || lv_obj_get_y(item) != tops[i]) {
            lv_obj_align(item, LV_ALIGN_TOP_MID, 0, tops[i]);
        }
    }
}

K_WORK_DEFINE(status_layout_work, status_layout_cb);

static void status_size_changed_cb(lv_event_t *event) {
    if (status_layout_screen != NULL) {
        k_work_submit_to_queue(zmk_display_work_q(), &status_layout_work);
    }
}

static void set_count_text(lv_obj_t *label, const char *text) {
    const lv_font_t *fonts[] = {
        &lv_font_montserrat_14, &lv_font_montserrat_10, &lv_font_montserrat_8,
    };
    lv_coord_t available = lv_obj_get_width(lv_obj_get_parent(label)) - 2;
    const lv_font_t *font = fonts[ARRAY_SIZE(fonts) - 1];
    for (size_t i = 0; i < ARRAY_SIZE(fonts); ++i) {
        if (lv_txt_get_width(text, strlen(text), fonts[i], 0, LV_TEXT_FLAG_NONE) <= available) {
            font = fonts[i];
            break;
        }
    }
    bool font_changed = lv_obj_get_style_text_font(label, LV_PART_MAIN) != font;
    bool text_changed = strcmp(lv_label_get_text(label), text) != 0;
    if (!font_changed && !text_changed) {
        return;
    }
    if (font_changed) {
        lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    }
    if (text_changed) {
        lv_label_set_text(label, text);
    }
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

static struct zmk_widget_output_status output_status_widget;
static struct zmk_widget_layer_status layer_status_widget;

/* Key press counter: counted on physical position events, displayed once per
 * minute, persisted to settings every 10 minutes and before going to sleep. */
static lv_obj_t *key_count_label;
static atomic_t key_press_count = ATOMIC_INIT(0);
static atomic_t key_count_saved = ATOMIC_INIT(0);
static struct zen_minute_counter minute_counter;
static atomic_t minute_delta = ATOMIC_INIT(0);
static atomic_t minute_sequence = ATOMIC_INIT(0);
static atomic_t minute_retries = ATOMIC_INIT(0);
static atomic_t minute_ready = ATOMIC_INIT(0);
static atomic_t minute_awake = ATOMIC_INIT(1);

#define KEY_COUNT_DISPLAY_INTERVAL K_MINUTES(1)
#define KEY_COUNT_SAVE_INTERVAL K_MINUTES(10)

static void minute_send_cb(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(minute_send_work, minute_send_cb);

static void minute_send_cb(struct k_work *work) {
    if (!atomic_get(&minute_ready) || atomic_dec(&minute_retries) <= 0) {
        atomic_set(&minute_retries, 0);
        return;
    }

    struct zmk_behavior_binding binding = {
        .behavior_dev = "zenmin",
        .param1 = (uint32_t)atomic_get(&minute_delta),
        .param2 = (uint32_t)atomic_get(&minute_sequence),
    };
    struct zmk_behavior_binding_event event = {0};
    int err = zmk_split_central_invoke_behavior(0, &binding, event, true);
    if (err) {
        LOG_WRN("Minute count queue failed: %d", err);
    }
    if (atomic_get(&minute_retries) > 0) {
        k_work_reschedule(&minute_send_work, K_SECONDS(10));
    }
}

static void key_count_display_cb(struct k_work *work) {
    if (work != NULL && !atomic_get(&minute_awake)) {
        return;
    }
    uint32_t count = (uint32_t)atomic_get(&key_press_count);
    char text[16];

    snprintf(text, sizeof(text), "Keys %lu", (unsigned long)count);
    set_count_text(key_count_label, text);

    if (work != NULL) {
        atomic_set(&minute_delta, (atomic_val_t)zen_minute_counter_complete(&minute_counter, count));
        atomic_inc(&minute_sequence);
        atomic_set(&minute_ready, 1);
        atomic_set(&minute_retries, 6);
        k_work_reschedule(&minute_send_work, K_NO_WAIT);
    }
}

K_WORK_DEFINE(key_count_display_work, key_count_display_cb);

static void key_count_save_cb(struct k_work *work) {
    uint32_t count = (uint32_t)atomic_get(&key_press_count);
    if (count == (uint32_t)atomic_get(&key_count_saved)) {
        return;
    }

    if (settings_save_one("zen_keys/count", &count, sizeof(count)) == 0) {
        atomic_set(&key_count_saved, (atomic_val_t)count);
    }
}

K_WORK_DEFINE(key_count_save_work, key_count_save_cb);

static void key_count_display_timer_cb(struct k_timer *timer) {
    k_work_submit_to_queue(zmk_display_work_q(), &key_count_display_work);
}

static void key_count_save_timer_cb(struct k_timer *timer) {
    k_work_submit(&key_count_save_work);
}

K_TIMER_DEFINE(key_count_display_timer, key_count_display_timer_cb, NULL);
K_TIMER_DEFINE(key_count_save_timer, key_count_save_timer_cb, NULL);

static int key_count_listener_cb(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->state) {
        atomic_inc(&key_press_count);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(key_count_listener, key_count_listener_cb);
ZMK_SUBSCRIPTION(key_count_listener, zmk_position_state_changed);

static int key_count_activity_cb(const zmk_event_t *eh) {
    struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);

    if (ev != NULL && ev->state == ZMK_ACTIVITY_SLEEP) {
        atomic_set(&minute_awake, 0);
        k_timer_stop(&key_count_display_timer);
        k_timer_stop(&key_count_save_timer);
        zen_minute_counter_reset(&minute_counter, (uint32_t)atomic_get(&key_press_count));
        atomic_set(&minute_ready, 0);
        k_work_cancel_delayable(&minute_send_work);
        key_count_save_cb(NULL);
    } else if (ev != NULL && ev->state == ZMK_ACTIVITY_ACTIVE && !atomic_get(&minute_awake)) {
        atomic_set(&minute_awake, 1);
        k_timer_start(&key_count_display_timer, KEY_COUNT_DISPLAY_INTERVAL,
                      KEY_COUNT_DISPLAY_INTERVAL);
        k_timer_start(&key_count_save_timer, KEY_COUNT_SAVE_INTERVAL, KEY_COUNT_SAVE_INTERVAL);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(key_count_activity_listener, key_count_activity_cb);
ZMK_SUBSCRIPTION(key_count_activity_listener, zmk_activity_state_changed);

static int key_count_battery_cb(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *ev =
        as_zmk_peripheral_battery_state_changed(eh);
    if (ev != NULL && ev->source == 0 && ev->state_of_charge > 0 &&
        atomic_get(&minute_ready)) {
        atomic_set(&minute_retries, 6);
        k_work_reschedule(&minute_send_work, K_SECONDS(2));
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(key_count_battery_listener, key_count_battery_cb);
ZMK_SUBSCRIPTION(key_count_battery_listener, zmk_peripheral_battery_state_changed);

static int key_count_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                                  void *cb_arg) {
    const char *next;
    uint32_t count = 0;

    if (settings_name_steq(name, "count", &next) && next == NULL && len == sizeof(count)) {
        if (read_cb(cb_arg, &count, sizeof(count)) == sizeof(count)) {
            atomic_set(&key_press_count, (atomic_val_t)count);
            atomic_set(&key_count_saved, (atomic_val_t)count);
        }
    }

    return 0;
}

static struct settings_handler key_count_settings = {
    .name = "zen_keys",
    .h_set = key_count_settings_set,
};

#else /* peripheral (right) side */

static struct zmk_widget_peripheral_status peripheral_status_widget;
static lv_obj_t *minute_label;
static atomic_t right_connected = ATOMIC_INIT(0);
static atomic_t right_valid = ATOMIC_INIT(0);
static atomic_t right_delta = ATOMIC_INIT(0);
static atomic_t right_sequence = ATOMIC_INIT(0);

static void minute_display_cb(struct k_work *work) {
    char text[16];
    if (atomic_get(&right_connected) && atomic_get(&right_valid)) {
        snprintf(text, sizeof(text), "+%lu", (unsigned long)(uint32_t)atomic_get(&right_delta));
    } else {
        strcpy(text, "+--");
    }
    set_count_text(minute_label, text);
}

K_WORK_DEFINE(minute_display_work, minute_display_cb);

void zen_minute_received(uint32_t delta, uint32_t sequence) {
    if (!zmk_split_bt_peripheral_is_connected() ||
        (atomic_get(&right_valid) && sequence <= (uint32_t)atomic_get(&right_sequence))) {
        return;
    }
    atomic_set(&right_delta, (atomic_val_t)delta);
    atomic_set(&right_sequence, (atomic_val_t)sequence);
    atomic_set(&right_valid, 1);
    if (minute_label != NULL && zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &minute_display_work);
    }
}

static int minute_connection_cb(const zmk_event_t *eh) {
    const struct zmk_split_peripheral_status_changed *ev =
        as_zmk_split_peripheral_status_changed(eh);
    if (ev != NULL) {
        atomic_set(&right_connected, ev->connected);
        if (!ev->connected) {
            atomic_set(&right_valid, 0);
        }
        if (minute_label != NULL && zmk_display_is_initialized()) {
            k_work_submit_to_queue(zmk_display_work_q(), &minute_display_work);
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(minute_connection_listener, minute_connection_cb);
ZMK_SUBSCRIPTION(minute_connection_listener, zmk_split_peripheral_status_changed);

#endif /* CONFIG_ZMK_SPLIT_ROLE_CENTRAL */

lv_obj_t *zmk_display_status_screen() {
    lv_obj_t *screen;
    screen = lv_obj_create(NULL);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    zmk_widget_battery_status_init(&battery_status_widget, screen);
    status_layout_items[0] = zmk_widget_battery_status_obj(&battery_status_widget);

    zmk_widget_output_status_init(&output_status_widget, screen);
    status_layout_items[1] = zmk_widget_output_status_obj(&output_status_widget);

    key_count_label = lv_label_create(screen);
    status_layout_items[2] = key_count_label;

    settings_register(&key_count_settings);
    settings_load_subtree("zen_keys");
    zen_minute_counter_reset(&minute_counter, (uint32_t)atomic_get(&key_press_count));
    key_count_display_cb(NULL);
    k_timer_start(&key_count_display_timer, KEY_COUNT_DISPLAY_INTERVAL,
                  KEY_COUNT_DISPLAY_INTERVAL);
    k_timer_start(&key_count_save_timer, KEY_COUNT_SAVE_INTERVAL, KEY_COUNT_SAVE_INTERVAL);

    lv_obj_t *LayersHeading;
    LayersHeading = lv_img_create(screen);
    lv_img_set_src(LayersHeading, &layers2);
    status_layout_items[3] = LayersHeading;

    zmk_widget_layer_status_init(&layer_status_widget, screen);
    lv_obj_set_style_text_font(zmk_widget_layer_status_obj(&layer_status_widget),
                               &lv_font_montserrat_16, LV_PART_MAIN);
    status_layout_items[4] = zmk_widget_layer_status_obj(&layer_status_widget);
    status_layout_count = 5;
#else
    zmk_widget_battery_status_init(&battery_status_widget, screen);
    status_layout_items[0] = zmk_widget_battery_status_obj(&battery_status_widget);

    zmk_widget_peripheral_status_init(&peripheral_status_widget, screen);
    status_layout_items[1] = zmk_widget_peripheral_status_obj(&peripheral_status_widget);

    minute_label = lv_label_create(screen);
    atomic_set(&right_connected, zmk_split_bt_peripheral_is_connected());
    minute_display_cb(NULL);
    status_layout_items[2] = minute_label;

    lv_obj_t *zenlogo_icon;
    zenlogo_icon = lv_img_create(screen);
    lv_img_set_src(zenlogo_icon, &zenlogo);
    status_layout_items[3] = zenlogo_icon;
    status_layout_count = 4;
#endif

    for (size_t i = 0; i < status_layout_count; ++i) {
        lv_obj_add_event_cb(status_layout_items[i], status_size_changed_cb, LV_EVENT_SIZE_CHANGED,
                            NULL);
    }
    status_layout_screen = screen;
    status_layout_cb(NULL);

    return screen;
}
