/* SPDX-License-Identifier: MIT */

#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci_types.h>

#include <zmk/ble.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>

static int zen_ble_profile_changed(const zmk_event_t *event) {
    if (as_zmk_ble_active_profile_changed(event) == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    int active = zmk_ble_active_profile_index();
    for (int index = 0; index < ZMK_BLE_PROFILE_COUNT; ++index) {
        if (index != active) {
            zmk_ble_prof_disconnect(index);
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(zen_ble_profile_guard, zen_ble_profile_changed);
ZMK_SUBSCRIPTION(zen_ble_profile_guard, zmk_ble_active_profile_changed);

static void zen_ble_connected(struct bt_conn *conn, uint8_t err) {
    struct bt_conn_info info;
    if (err != 0 || bt_conn_get_info(conn, &info) != 0 ||
        info.role != BT_CONN_ROLE_PERIPHERAL) {
        return;
    }

    int profile = zmk_ble_profile_index(bt_conn_get_dst(conn));
    if (profile >= 0 && profile != zmk_ble_active_profile_index()) {
        bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
}

BT_CONN_CB_DEFINE(zen_ble_profile_connection_guard) = {
    .connected = zen_ble_connected,
};
