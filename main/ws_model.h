#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "device_manager.h"
#include "zb_events.h"

typedef struct {
    const char *name;
    const char *unit;
    bool known;
} ws_attr_meta_t;

void ws_json_append(char **p, char *end, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void ws_json_append_string(char **p, char *end, const char *value);

void ws_model_append_device_id(char **p, char *end, uint64_t ieee);
void ws_model_append_inventory_device(char **p, char *end,
                                      const device_record_t *dev);
void ws_model_append_state_device(char **p, char *end,
                                  const device_record_t *dev);
bool ws_model_attr_meta(uint16_t cluster_id, uint16_t attr_id,
                        ws_attr_meta_t *out);
bool ws_model_append_attr_value_object(char **p, char *end,
                                       uint16_t cluster_id, uint16_t attr_id,
                                       uint8_t attr_type,
                                       const uint8_t value[8],
                                       uint32_t ts);
void ws_model_append_event_change(char **p, char *end, const zb_event_t *evt);

