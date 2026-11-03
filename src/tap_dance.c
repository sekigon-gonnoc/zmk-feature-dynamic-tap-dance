/* SPDX-License-Identifier: MIT */

#define DT_DRV_COMPAT sekigon_dynamic_behavior_tap_dance

#include <errno.h>
#include <string.h>

#include <drivers/behavior.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/sys/util.h>

#include <zmk/behavior.h>
#include <zmk/dynamic_settings/store.h>
#include <zmk/dynamic_tap_dance/tap_dance.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>

LOG_MODULE_REGISTER(dynamic_tap_dance, CONFIG_ZMK_LOG_LEVEL);

#define TD_RECORD_VERSION 1
#define TD_OUTPUT_OFFSET 5
#define TD_OUTPUT_SIZE 10
#define TD_SLOT_COUNT CONFIG_ZMK_DYNAMIC_TAP_DANCE_MAX_SLOTS
#define TD_ACTIVE_COUNT CONFIG_ZMK_DYNAMIC_TAP_DANCE_MAX_ACTIVE

BUILD_ASSERT(TD_SLOT_COUNT > 0, "Runtime Tap Dance needs at least one dispatcher");
BUILD_ASSERT(TD_SLOT_COUNT <= CONFIG_ZMK_DYNAMIC_TAP_DANCE_MAX_SLOTS,
             "Runtime Tap Dance slot count exceeds its configured maximum");
BUILD_ASSERT(!DT_HAS_COMPAT_STATUS_OKAY(zmk_behavior_tap_dance),
             "Static zmk,behavior-tap-dance cannot be used with Nemu-link "
             "runtime Tap Dance");

struct td_slot {
    const struct device *device;
    uint8_t index;
    uint8_t record[ZMK_DYNAMIC_TAP_DANCE_RECORD_SIZE];
    struct dynamic_tap_dance_config config;
    bool initialized;
    bool resolved;
};

enum td_phase {
    TD_IDLE,
    TD_FIRST_DOWN,
    TD_FIRST_UP,
    TD_SECOND_DOWN,
    TD_HOLD,
    TD_TAP_HOLD,
};

struct td_active {
    struct k_work_delayable timer;
    struct td_slot *slot;
    struct zmk_behavior_binding_event event;
    struct zmk_behavior_binding held_binding;
    enum td_phase phase;
    int64_t deadline;
    uint8_t source;
    uint32_t position;
    uint8_t output_index;
    bool active;
    bool output_held;
};

#define TD_SLOT_ENTRY(inst)                                                                        \
    [DT_PROP(DT_DRV_INST(inst), slot_index)] = {                                                   \
        .device = DEVICE_DT_INST_GET(inst),                                                        \
        .index = DT_PROP(DT_DRV_INST(inst), slot_index),                                           \
        .record = {TD_RECORD_VERSION},                                                             \
    },
static struct td_slot td_slots[TD_SLOT_COUNT] = {DT_INST_FOREACH_STATUS_OKAY(TD_SLOT_ENTRY)};
#undef TD_SLOT_ENTRY

static struct td_active td_active_states[TD_ACTIVE_COUNT];
K_MUTEX_DEFINE(td_mutex);

static uint16_t behavior_identity(const char *name) {
    uint32_t hash = 2166136261u;
    for (const uint8_t *p = (const uint8_t *)name; *p; p++) {
        hash = (hash ^ *p) * 16777619u;
    }
    hash ^= hash >> 16;
    return (uint16_t)hash;
}

static const struct device *resolve_behavior(uint16_t identity) {
    const struct device *match = NULL;
    STRUCT_SECTION_FOREACH(zmk_behavior_ref, ref) {
        if (!ref->device) {
            continue;
        }
        bool matches = behavior_identity(ref->device->name) == identity;
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
        if (ref->metadata.display_name &&
            behavior_identity(ref->metadata.display_name) == identity) {
            matches = true;
        }
#endif
        if (!matches) {
            continue;
        }
        if (match && match != ref->device) {
            return NULL;
        }
        match = ref->device;
    }
    return match && device_is_ready(match) ? match : NULL;
}

static bool is_dispatcher(const struct device *device) {
    for (size_t i = 0; i < ARRAY_SIZE(td_slots); i++) {
        if (td_slots[i].device == device) {
            return true;
        }
    }
    return false;
}

static int decode_record(const uint8_t *record, size_t length,
                         struct dynamic_tap_dance_config *config, bool resolve) {
    if (!record || !config || length != ZMK_DYNAMIC_TAP_DANCE_RECORD_SIZE ||
        record[0] != TD_RECORD_VERSION || record[1] > 1 || (record[4] & 0xf0) != 0) {
        return -EINVAL;
    }
    struct dynamic_tap_dance_config staged = {.enabled = record[1] == 1,
                                              .term_ms = sys_get_le16(&record[2])};
    if (!staged.enabled) {
        if (record[1] != 0 || record[2] || record[3] || record[4]) {
            return -EINVAL;
        }
        for (size_t i = TD_OUTPUT_OFFSET; i < length; i++) {
            if (record[i]) {
                return -EINVAL;
            }
        }
        *config = staged;
        return 0;
    }
    if (staged.term_ms < 10 || staged.term_ms > 500) {
        return -ERANGE;
    }
    const uint8_t mask = record[4];
    for (uint8_t i = 0; i < ZMK_DYNAMIC_TAP_DANCE_OUTPUTS; i++) {
        const uint8_t *entry = &record[TD_OUTPUT_OFFSET + i * TD_OUTPUT_SIZE];
        if ((mask & BIT(i)) == 0) {
            for (size_t j = 0; j < TD_OUTPUT_SIZE; j++) {
                if (entry[j]) {
                    return -EINVAL;
                }
            }
            continue;
        }
        const uint16_t identity = sys_get_le16(entry);
        if (resolve) {
            const struct device *behavior = resolve_behavior(identity);
            if (!behavior || is_dispatcher(behavior)) {
                return -ENOENT;
            }
            struct zmk_behavior_binding binding = {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
                .local_id = zmk_behavior_get_local_id(behavior->name),
#endif
                .behavior_dev = behavior->name,
                .param1 = sys_get_le32(entry + 2),
                .param2 = sys_get_le32(entry + 6),
            };
            const struct behavior_driver_api *api = behavior->api;
            if (!api || !api->binding_pressed || !api->binding_released ||
                zmk_behavior_validate_binding(&binding)) {
                return -EINVAL;
            }
            staged.outputs[i] =
                (struct dynamic_tap_dance_output){.configured = true, .binding = binding};
        }
    }
    *config = staged;
    return 0;
}

static struct td_slot *slot_for_group(const struct dynamic_setting_group_descriptor *group,
                                      const void *context) {
    ARG_UNUSED(context);
    if (!group) {
        return NULL;
    }
    return group->slot_index < ARRAY_SIZE(td_slots) ? &td_slots[group->slot_index] : NULL;
}

dynamic_setting_record_on_change_fn dynamic_tap_dance_record_changed;
int dynamic_tap_dance_record_changed(const struct dynamic_setting_group_descriptor *group,
                                     const uint8_t *record, size_t length,
                                     enum dynamic_setting_change_reason reason,
                                     const void *context) {
    struct td_slot *slot = slot_for_group(group, context);
    if (!slot) {
        return -EINVAL;
    }
    struct dynamic_tap_dance_config staged = {0};
    int err = decode_record(record, length, &staged, reason != DYNAMIC_SETTING_CHANGE_BOOT_LOAD);
    if (err) {
        return err;
    }
    k_mutex_lock(&td_mutex, K_FOREVER);
    for (size_t i = 0; i < ARRAY_SIZE(td_active_states); i++) {
        if (td_active_states[i].active && td_active_states[i].slot == slot) {
            k_mutex_unlock(&td_mutex);
            return -EBUSY;
        }
    }
    memcpy(slot->record, record, length);
    slot->config = staged;
    slot->initialized = true;
    slot->resolved = reason != DYNAMIC_SETTING_CHANGE_BOOT_LOAD || !staged.enabled;
    k_mutex_unlock(&td_mutex);
    return 0;
}

static struct td_active *find_active(uint8_t source, uint32_t position) {
    for (size_t i = 0; i < ARRAY_SIZE(td_active_states); i++) {
        if (td_active_states[i].active && td_active_states[i].source == source &&
            td_active_states[i].position == position) {
            return &td_active_states[i];
        }
    }
    return NULL;
}

static struct zmk_behavior_binding_event output_event(const struct td_active *state,
                                                      int64_t timestamp) {
    struct zmk_behavior_binding_event event = state->event;
    event.timestamp = timestamp;
    return event;
}

static void clear_active(struct td_active *state) {
    state->active = false;
    state->phase = TD_IDLE;
    state->slot = NULL;
    state->output_held = false;
}

static void invoke_tap(const struct zmk_behavior_binding *binding,
                       struct zmk_behavior_binding_event event) {
    zmk_behavior_invoke_binding(binding, event, true);
    zmk_behavior_invoke_binding(binding, event, false);
}

static void decide_active(struct td_active *state, uint8_t output_index, bool hold,
                          int64_t timestamp) {
    const bool configured = state->slot->config.outputs[output_index].configured;
    struct zmk_behavior_binding binding = state->slot->config.outputs[output_index].binding;
    struct zmk_behavior_binding_event event = output_event(state, timestamp);
    if (hold) {
        state->phase = output_index == 1 ? TD_HOLD : TD_TAP_HOLD;
        state->output_index = output_index;
        int err = configured ? zmk_behavior_invoke_binding(&binding, event, true) : -ENOENT;
        if (configured && err == 0) {
            state->held_binding = binding;
            state->output_held = true;
        }
        LOG_INF("hold decision source=%u position=%u output=%u configured=%d "
                "invoke=%d held=%d",
                state->source, state->position, output_index, configured, err, state->output_held);
    } else {
        clear_active(state);
        if (configured) {
            invoke_tap(&binding, event);
        }
    }
}

static void timeout_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct td_active *state = CONTAINER_OF(dwork, struct td_active, timer);
    k_mutex_lock(&td_mutex, K_FOREVER);
    if (!state->active) {
        k_mutex_unlock(&td_mutex);
        return;
    }
    const int64_t now = k_uptime_get();
    if (now < state->deadline) {
        k_work_schedule(&state->timer, K_MSEC(state->deadline - now));
        k_mutex_unlock(&td_mutex);
        return;
    }
    LOG_INF("term expired source=%u position=%u phase=%u late_ms=%lld", state->source,
            state->position, state->phase, now - state->deadline);
    if (state->phase == TD_FIRST_DOWN) {
        decide_active(state, 1, true, state->deadline);
    } else if (state->phase == TD_FIRST_UP) {
        decide_active(state, 0, false, state->deadline);
    } else if (state->phase == TD_SECOND_DOWN) {
        decide_active(state, 3, true, state->deadline);
    }
    k_mutex_unlock(&td_mutex);
}

static void start_timer(struct td_active *state, int64_t timestamp) {
    state->deadline = timestamp + state->slot->config.term_ms;
    int64_t remaining = state->deadline - k_uptime_get();
    k_work_reschedule(&state->timer, K_MSEC(MAX(remaining, 0)));
}

static int process_dispatch(struct td_slot *slot, struct zmk_behavior_binding_event event,
                            uint8_t source, uint32_t position, bool pressed) {
    struct td_active *state;
    k_mutex_lock(&td_mutex, K_FOREVER);
    state = find_active(source, position);
    if (pressed) {
        if (state && event.timestamp >= state->deadline &&
            (state->phase == TD_FIRST_UP || state->phase == TD_FIRST_DOWN ||
             state->phase == TD_SECOND_DOWN)) {
            if (state->phase == TD_FIRST_UP) {
                decide_active(state, 0, false, state->deadline);
            } else if (state->phase == TD_FIRST_DOWN) {
                decide_active(state, 1, true, state->deadline);
            } else {
                decide_active(state, 3, true, state->deadline);
            }
            state = NULL;
        }
        if (!state) {
            for (size_t i = 0; i < ARRAY_SIZE(td_active_states); i++) {
                if (!td_active_states[i].active) {
                    state = &td_active_states[i];
                    break;
                }
            }
            if (!state || !slot->initialized || !slot->config.enabled) {
                k_mutex_unlock(&td_mutex);
                return state ? 0 : -ENOMEM;
            }
            state->slot = slot;
            state->source = source;
            state->position = position;
            state->event = event;
            state->phase = TD_FIRST_DOWN;
            state->active = true;
            state->output_held = false;
            start_timer(state, event.timestamp);
            k_mutex_unlock(&td_mutex);
            return 0;
        }
        if (state->phase == TD_FIRST_UP) {
            state->phase = TD_SECOND_DOWN;
            state->event = event;
            start_timer(state, event.timestamp);
        }
        k_mutex_unlock(&td_mutex);
        return 0;
    }
    if (!state) {
        k_mutex_unlock(&td_mutex);
        return 0;
    }
    if (event.timestamp >= state->deadline &&
        (state->phase == TD_FIRST_DOWN || state->phase == TD_FIRST_UP ||
         state->phase == TD_SECOND_DOWN)) {
        if (state->phase == TD_FIRST_UP) {
            decide_active(state, 0, false, state->deadline);
        } else if (state->phase == TD_FIRST_DOWN) {
            decide_active(state, 1, true, state->deadline);
        } else {
            decide_active(state, 3, true, state->deadline);
        }
        if (!state->active) {
            k_mutex_unlock(&td_mutex);
            return 0;
        }
    }
    if (state->phase == TD_FIRST_DOWN) {
        state->phase = TD_FIRST_UP;
        k_mutex_unlock(&td_mutex);
        return 0;
    }
    if (state->phase == TD_SECOND_DOWN) {
        decide_active(state, 2, false, event.timestamp);
        k_mutex_unlock(&td_mutex);
        return 0;
    }
    if ((state->phase == TD_HOLD || state->phase == TD_TAP_HOLD) && state->output_held) {
        struct zmk_behavior_binding binding = state->held_binding;
        struct zmk_behavior_binding_event output = output_event(state, event.timestamp);
        LOG_INF("hold release source=%u position=%u output=%u", state->source, state->position,
                state->output_index);
        clear_active(state);
        k_mutex_unlock(&td_mutex);
        zmk_behavior_invoke_binding(&binding, output, false);
        return 0;
    }
    if (state->phase == TD_HOLD || state->phase == TD_TAP_HOLD) {
        clear_active(state);
    }
    k_mutex_unlock(&td_mutex);
    return 0;
}

static int binding_pressed(struct zmk_behavior_binding *binding,
                           struct zmk_behavior_binding_event event) {
    const struct device *device = zmk_behavior_get_binding(binding->behavior_dev);
    for (size_t i = 0; i < ARRAY_SIZE(td_slots); i++) {
        if (td_slots[i].device == device) {
            struct dynamic_tap_dance_config config;
            int err = dynamic_tap_dance_slot_get(i, &config);
            if (err) {
                return err;
            }
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
            return process_dispatch(&td_slots[i], event, event.source, event.position, true);
#else
            return process_dispatch(&td_slots[i], event, 0, event.position, true);
#endif
        }
    }
    return -ENODEV;
}

static int binding_released(struct zmk_behavior_binding *binding,
                            struct zmk_behavior_binding_event event) {
    const struct device *device = zmk_behavior_get_binding(binding->behavior_dev);
    ARG_UNUSED(device);
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    return process_dispatch(NULL, event, event.source, event.position, false);
#else
    return process_dispatch(NULL, event, 0, event.position, false);
#endif
}

void dynamic_tap_dance_position_interrupt(uint8_t source, uint32_t position, int64_t timestamp) {
#if !IS_ENABLED(CONFIG_ZMK_SPLIT)
    source = 0;
#endif
    k_mutex_lock(&td_mutex, K_FOREVER);
    for (size_t i = 0; i < ARRAY_SIZE(td_active_states); i++) {
        struct td_active *state = &td_active_states[i];
        if (!state->active || (state->source == source && state->position == position)) {
            continue;
        }
        if (state->phase == TD_FIRST_UP) {
            decide_active(state, 0, false, timestamp);
        } else if (state->phase == TD_FIRST_DOWN) {
            decide_active(state, 1, true, timestamp);
        } else if (state->phase == TD_SECOND_DOWN) {
            decide_active(state, 3, true, timestamp);
        }
    }
    k_mutex_unlock(&td_mutex);
}

struct td_cleanup {
    struct zmk_behavior_binding binding;
    struct zmk_behavior_binding_event event;
};

static void cleanup_active_states(void) {
    struct td_cleanup cleanup[TD_ACTIVE_COUNT];
    size_t cleanup_count = 0;
    k_mutex_lock(&td_mutex, K_FOREVER);
    for (size_t i = 0; i < ARRAY_SIZE(td_active_states); i++) {
        struct td_active *state = &td_active_states[i];
        if (!state->active) {
            continue;
        }
        k_work_cancel_delayable(&state->timer);
        if (state->output_held) {
            cleanup[cleanup_count++] = (struct td_cleanup){
                .binding = state->held_binding,
                .event = output_event(state, k_uptime_get()),
            };
        }
        clear_active(state);
    }
    k_mutex_unlock(&td_mutex);
    for (size_t i = 0; i < cleanup_count; i++) {
        zmk_behavior_invoke_binding(&cleanup[i].binding, cleanup[i].event, false);
    }
}

static int endpoint_listener(const zmk_event_t *event) {
    ARG_UNUSED(event);
    cleanup_active_states();
    return ZMK_EV_EVENT_BUBBLE;
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT)
static int split_status_listener(const zmk_event_t *event) {
    const struct zmk_split_peripheral_status_changed *status =
        as_zmk_split_peripheral_status_changed(event);
    if (status && !status->connected) {
        cleanup_active_states();
    }
    return ZMK_EV_EVENT_BUBBLE;
}
#endif

ZMK_LISTENER(dynamic_tap_dance_endpoint, endpoint_listener);
ZMK_SUBSCRIPTION(dynamic_tap_dance_endpoint, zmk_endpoint_changed);
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
ZMK_LISTENER(dynamic_tap_dance_split, split_status_listener);
ZMK_SUBSCRIPTION(dynamic_tap_dance_split, zmk_split_peripheral_status_changed);
#endif

static const struct behavior_driver_api td_driver_api = {
    .binding_pressed = binding_pressed,
    .binding_released = binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

static int slot_init(const struct device *dev) {
    ARG_UNUSED(dev);
    return 0;
}

#define TD_SLOT_INDEX_ASSERT(inst)                                                                 \
    BUILD_ASSERT(DT_PROP(DT_DRV_INST(inst), slot_index) < TD_SLOT_COUNT,                           \
                 "Tap Dance slot-index is outside the configured slot table");
DT_INST_FOREACH_STATUS_OKAY(TD_SLOT_INDEX_ASSERT)
#undef TD_SLOT_INDEX_ASSERT

#define TD_DEFINE(inst)                                                                            \
    BEHAVIOR_DT_INST_DEFINE(inst, slot_init, NULL, NULL, NULL, POST_KERNEL,                        \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &td_driver_api);
DT_INST_FOREACH_STATUS_OKAY(TD_DEFINE)
#undef TD_DEFINE

static int init_active_states(void) {
    for (size_t i = 0; i < ARRAY_SIZE(td_active_states); i++) {
        k_work_init_delayable(&td_active_states[i].timer, timeout_handler);
    }
    return 0;
}
SYS_INIT(init_active_states, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

uint8_t dynamic_tap_dance_slot_count(void) { return ARRAY_SIZE(td_slots); }

static int initialize_slot_from_settings(struct td_slot *slot) {
    const struct dynamic_setting_group_descriptor *group = NULL;
    for (size_t i = 0; i < dynamic_settings_shared_group_count(); i++) {
        const struct dynamic_setting_group_descriptor *candidate =
            dynamic_settings_shared_group_at(i);
        if (candidate && candidate->slot_index == slot->index &&
            (candidate->flags & DYNAMIC_SETTING_GROUP_FLAG_RECORD)) {
            group = candidate;
            break;
        }
    }
    if (!group) {
        return -ENOENT;
    }

    uint8_t record[ZMK_DYNAMIC_TAP_DANCE_RECORD_SIZE];
    size_t length = 0;
    int err = dynamic_settings_record_read(group->id, 0, record, sizeof(record), &length, NULL);
    if (err) {
        return err;
    }
    return dynamic_tap_dance_record_changed(group, record, length, DYNAMIC_SETTING_CHANGE_BOOT_LOAD,
                                            slot->device);
}

int dynamic_tap_dance_slot_get(uint8_t index, struct dynamic_tap_dance_config *config) {
    if (index >= ARRAY_SIZE(td_slots) || !config) {
        return -EINVAL;
    }
    struct td_slot *slot = &td_slots[index];
    k_mutex_lock(&td_mutex, K_FOREVER);
    if (!slot->initialized) {
        k_mutex_unlock(&td_mutex);
        int err = initialize_slot_from_settings(slot);
        if (err) {
            return err;
        }
        k_mutex_lock(&td_mutex, K_FOREVER);
    }
    if (slot->config.enabled && !slot->resolved) {
        struct dynamic_tap_dance_config resolved = {0};
        int err = decode_record(slot->record, sizeof(slot->record), &resolved, true);
        if (err) {
            k_mutex_unlock(&td_mutex);
            return err;
        }
        slot->config = resolved;
        slot->resolved = true;
    }
    *config = slot->config;
    k_mutex_unlock(&td_mutex);
    return 0;
}
