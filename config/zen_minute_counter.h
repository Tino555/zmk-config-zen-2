#pragma once

#include <stdint.h>

struct zen_minute_counter {
    uint32_t start;
};

static inline void zen_minute_counter_reset(struct zen_minute_counter *counter, uint32_t count) {
    counter->start = count;
}

static inline uint32_t zen_minute_counter_complete(struct zen_minute_counter *counter,
                                                    uint32_t count) {
    uint32_t delta = count - counter->start;
    counter->start = count;
    return delta;
}
