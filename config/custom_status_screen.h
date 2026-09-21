/*
 *
 * Copyright (c) 2021 Darryl deHaan
 * SPDX-License-Identifier: MIT
 *
 */

#pragma once

#include <lvgl.h>
#include <stdint.h>

lv_obj_t *zmk_display_status_screen();
void zen_minute_received(uint32_t delta, uint32_t sequence);
