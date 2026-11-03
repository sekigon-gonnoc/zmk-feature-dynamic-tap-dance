/* SPDX-License-Identifier: MIT */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zmk/behavior.h>

#define ZMK_DYNAMIC_TAP_DANCE_OUTPUTS 4
#define ZMK_DYNAMIC_TAP_DANCE_RECORD_SIZE 45

struct dynamic_tap_dance_output {
    bool configured;
    struct zmk_behavior_binding binding;
};

struct dynamic_tap_dance_config {
    bool enabled;
    uint16_t term_ms;
    struct dynamic_tap_dance_output outputs[ZMK_DYNAMIC_TAP_DANCE_OUTPUTS];
};

uint8_t dynamic_tap_dance_slot_count(void);
int dynamic_tap_dance_slot_get(uint8_t slot, struct dynamic_tap_dance_config *config);
void dynamic_tap_dance_position_interrupt(uint8_t source, uint32_t position, int64_t timestamp);
