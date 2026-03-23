#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "zb_persistence.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*zb_coordinator_event_cb_t)(const char *event_name);

esp_err_t zb_coordinator_init(const zb_persist_state_t *persist_state, zb_coordinator_event_cb_t event_cb);
esp_err_t zb_coordinator_set_permit_join(bool enable);
esp_err_t zb_coordinator_poll(void);
esp_err_t zb_coordinator_get_runtime_state(zb_persist_state_t *out_state);

#ifdef __cplusplus
}
#endif
