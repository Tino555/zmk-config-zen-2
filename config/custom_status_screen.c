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

#include <zephyr/bluetooth/services/bas.h>

#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/split/central.h>

#include <dt-bindings/zmk/hid_indicators.h>

#include "widgets/output_status.h"
#include "widgets/layer_status.h"
#include "widgets/peripheral_status.h"
#include "custom_status_screen.h"

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

LV_IMG_DECLARE(zenlogo);
LV_IMG_DECLARE(layers2);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

static struct zmk_widget_output_status output_status_widget;
static struct zmk_widget_layer_status layer_status_widget;

/* Combined battery: L<left> R<right>, right shown as -- until the first
 * peripheral battery report arrives. */
static lv_obj_t *combo_batt_label;
static bool combo_batt_right_valid;

struct combo_batt_state {
    uint8_t left_level;
    uint8_t right_level;
    bool right_valid;
};

static struct combo_batt_state combo_batt_get_state(const zmk_event_t *eh) {
    struct combo_batt_state state = {
        .left_level = bt_bas_get_battery_level(),
        .right_level = 0,
        .right_valid = combo_batt_right_valid,
    };

    if (state.right_valid) {
        zmk_split_central_get_peripheral_battery_level(0, &state.right_level);
    }

    return state;
}

static void combo_batt_update_cb(struct combo_batt_state state) {
    char text[16];

    if (state.right_valid) {
        snprintf(text, sizeof(text), "L%d R%d", state.left_level, state.right_level);
    } else {
        snprintf(text, sizeof(text), "L%d R--", state.left_level);
    }

    lv_label_set_text(combo_batt_label, text);
}

ZMK_DISPLAY_WIDGET_LISTENER(combo_batt_listener, struct combo_batt_state, combo_batt_update_cb,
                            combo_batt_get_state)
ZMK_SUBSCRIPTION(combo_batt_listener, zmk_battery_state_changed);
ZMK_SUBSCRIPTION(combo_batt_listener, zmk_peripheral_battery_state_changed);

static int combo_batt_mark_right_valid_cb(const zmk_event_t *eh) {
    combo_batt_right_valid = true;
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(combo_batt_mark_right_valid, combo_batt_mark_right_valid_cb);
ZMK_SUBSCRIPTION(combo_batt_mark_right_valid, zmk_peripheral_battery_state_changed);

/* Key press counter: counted on physical position events, displayed once per
 * minute, persisted to settings every 10 minutes and before going to sleep. */
static lv_obj_t *key_count_label;
static atomic_t key_press_count = ATOMIC_INIT(0);
static atomic_t key_count_dirty = ATOMIC_INIT(0);
static atomic_t key_count_saved = ATOMIC_INIT(0);

#define KEY_COUNT_DISPLAY_INTERVAL K_MINUTES(1)
#define KEY_COUNT_SAVE_INTERVAL K_MINUTES(10)

static void key_count_display_cb(struct k_work *work) {
    char text[12];

    snprintf(text, sizeof(text), "%lu", (unsigned long)atomic_get(&key_press_count));
    if (strcmp(lv_label_get_text(key_count_label), text) != 0) {
        lv_label_set_text(key_count_label, text);
    }
}

K_WORK_DEFINE(key_count_display_work, key_count_display_cb);

static void key_count_save_cb(struct k_work *work) {
    if (!atomic_get(&key_count_dirty)) {
        return;
    }

    uint32_t count = (uint32_t)atomic_get(&key_press_count);
    if (settings_save_one("zen_keys/count", &count, sizeof(count)) == 0) {
        atomic_set(&key_count_saved, (atomic_val_t)count);
        atomic_set(&key_count_dirty, 0);
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
        atomic_set(&key_count_dirty, 1);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(key_count_listener, key_count_listener_cb);
ZMK_SUBSCRIPTION(key_count_listener, zmk_position_state_changed);

static int key_count_activity_cb(const zmk_event_t *eh) {
    struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);

    if (ev != NULL && ev->state == ZMK_ACTIVITY_SLEEP && atomic_get(&key_count_dirty)) {
        key_count_save_cb(NULL);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(key_count_activity_listener, key_count_activity_cb);
ZMK_SUBSCRIPTION(key_count_activity_listener, zmk_activity_state_changed);

static int key_count_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                                  void *cb_arg) {
    const char *next;
    uint32_t count = 0;

    if (settings_name_steq(name, "count", &next) && next == NULL && len == sizeof(count)) {
        if (read_cb(cb_arg, &count, sizeof(count)) == sizeof(count)) {
            atomic_set(&key_press_count, (atomic_val_t)count);
            atomic_set(&key_count_saved, (atomic_val_t)count);
            atomic_set(&key_count_dirty, 0);
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

static lv_obj_t *status_screen;
static lv_obj_t *locks_label;
static zmk_hid_indicators_t locks_indicators;

static void locks_display_cb(struct k_work *work) {
    char line1[8] = "";
    char text[12] = "";

    if (locks_indicators & HID_INDICATOR_CAPS_LOCK) {
        strcat(line1, "CAP");
    }
    if (locks_indicators & HID_INDICATOR_NUM_LOCK) {
        if (line1[0] != '\0') {
            strcat(line1, " ");
        }
        strcat(line1, "NUM");
    }
    if (line1[0] != '\0') {
        strcpy(text, line1);
    }
    if (locks_indicators & HID_INDICATOR_SCROLL_LOCK) {
        if (text[0] != '\0') {
            strcat(text, "\n");
        }
        strcat(text, "SCR");
    }

    lv_label_set_text(locks_label, text);
}

K_WORK_DEFINE(locks_display_work, locks_display_cb);

static int locks_listener_cb(const zmk_event_t *eh) {
    const struct zmk_hid_indicators_changed *ev = as_zmk_hid_indicators_changed(eh);

    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    locks_indicators = ev->indicators;

    if (zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &locks_display_work);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(locks_listener, locks_listener_cb);
ZMK_SUBSCRIPTION(locks_listener, zmk_hid_indicators_changed);

/* Redraw the whole screen on any state change so the IL0323 driver always
 * receives a full frame, keeping battery/connection/logo blacks even. */
static void full_refresh_cb(struct k_work *work) {
    if (status_screen != NULL) {
        lv_obj_invalidate(status_screen);
    }
}

K_WORK_DEFINE(full_refresh_work, full_refresh_cb);

static int full_refresh_listener_cb(const zmk_event_t *eh) {
    if (zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &full_refresh_work);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(full_refresh_listener, full_refresh_listener_cb);
ZMK_SUBSCRIPTION(full_refresh_listener, zmk_split_peripheral_status_changed);
ZMK_SUBSCRIPTION(full_refresh_listener, zmk_hid_indicators_changed);

#endif /* CONFIG_ZMK_SPLIT_ROLE_CENTRAL */

lv_obj_t *zmk_display_status_screen() {
    lv_obj_t *screen;
    screen = lv_obj_create(NULL);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    combo_batt_label = lv_label_create(screen);
    lv_obj_set_style_text_font(combo_batt_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(combo_batt_label, LV_ALIGN_TOP_MID, 0, 2);
    combo_batt_listener_init();

    zmk_widget_output_status_init(&output_status_widget, screen);
    lv_obj_align(zmk_widget_output_status_obj(&output_status_widget), LV_ALIGN_TOP_MID, 0, 24);

    key_count_label = lv_label_create(screen);
    lv_obj_set_style_text_font(key_count_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(key_count_label, LV_ALIGN_BOTTOM_MID, 0, -48);

    settings_register(&key_count_settings);
    settings_load_subtree("zen_keys");
    key_count_display_cb(NULL);
    k_timer_start(&key_count_display_timer, KEY_COUNT_DISPLAY_INTERVAL,
                  KEY_COUNT_DISPLAY_INTERVAL);
    k_timer_start(&key_count_save_timer, KEY_COUNT_SAVE_INTERVAL, KEY_COUNT_SAVE_INTERVAL);

    lv_obj_t *LayersHeading;
    LayersHeading = lv_img_create(screen);
    lv_obj_align(LayersHeading, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_img_set_src(LayersHeading, &layers2);

    zmk_widget_layer_status_init(&layer_status_widget, screen);
    lv_obj_set_style_text_font(zmk_widget_layer_status_obj(&layer_status_widget),
                               &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(zmk_widget_layer_status_obj(&layer_status_widget), LV_ALIGN_BOTTOM_MID, 0, -5);
#else
    status_screen = screen;

    zmk_widget_peripheral_status_init(&peripheral_status_widget, screen);
    lv_obj_align(zmk_widget_peripheral_status_obj(&peripheral_status_widget), LV_ALIGN_TOP_MID, 0,
                 2);

    locks_label = lv_label_create(screen);
    lv_obj_set_style_text_font(locks_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(locks_label, LV_ALIGN_TOP_MID, 0, 42);
    locks_display_cb(NULL);

    lv_obj_t *zenlogo_icon;
    zenlogo_icon = lv_img_create(screen);
    lv_img_set_src(zenlogo_icon, &zenlogo);
    lv_obj_align(zenlogo_icon, LV_ALIGN_BOTTOM_MID, 0, -5);
#endif

    return screen;
}
