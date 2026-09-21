/* SPDX-License-Identifier: MIT */
#pragma once

#include <stddef.h>
#include <stdint.h>

static inline void zen_screen_layout(int32_t screen_height, const int32_t *heights, size_t count,
                                     int32_t *tops) {
    int32_t used = 0;
    for (size_t i = 0; i < count; ++i) {
        used += heights[i];
    }

    int32_t space = screen_height - used;
    if (space < 0) {
        space = 0;
    }

    int32_t previous_heights = 0;
    for (size_t i = 0; i < count; ++i) {
        tops[i] = previous_heights + space * (i + 1) / (count + 1);
        previous_heights += heights[i];
    }
}
