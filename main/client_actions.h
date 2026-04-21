#pragma once
#include <stdbool.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Client action helpers.
//
// Transport-neutral entry points for commands coming from external clients.
// These keep the Zigbee work that external transports will need, without
// owning message parsing or response formatting.
// ---------------------------------------------------------------------------

typedef enum {
    CLIENT_ACTION_OK = 0,
    CLIENT_ACTION_INVALID_ARG,
    CLIENT_ACTION_DEVICE_NOT_FOUND,
    CLIENT_ACTION_UNSUPPORTED,
    CLIENT_ACTION_BUSY,
} client_action_result_t;

client_action_result_t client_actions_set_state(const char *device_id,
                                                const char *state,
                                                uint32_t delay_ms);

client_action_result_t client_actions_set_brightness(const char *device_id,
                                                     int brightness,
                                                     uint32_t delay_ms);

client_action_result_t client_actions_read_attribute(const char *device_id,
                                                     uint16_t cluster_id,
                                                     uint16_t attr_id,
                                                     uint32_t delay_ms);

client_action_result_t client_actions_read_named_attribute(const char *device_id,
                                                           const char *name,
                                                           uint32_t delay_ms);

client_action_result_t client_actions_set_permit_join(uint8_t duration_s);

client_action_result_t client_actions_interview_device(const char *device_id);

client_action_result_t client_actions_configure_device(const char *device_id);

client_action_result_t client_actions_rename_device(const char *from_id,
                                                    const char *new_name);
