/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT zmk_behavior_zen_minute

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include "custom_status_screen.h"

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
static int zen_minute_pressed(struct zmk_behavior_binding *binding,
                              struct zmk_behavior_binding_event event) {
    zen_minute_received(binding->param1, binding->param2);
    return 0;
}

static const struct behavior_driver_api zen_minute_api = {
    .binding_pressed = zen_minute_pressed,
    .locality = BEHAVIOR_LOCALITY_EVENT_SOURCE,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &zen_minute_api);
#endif
