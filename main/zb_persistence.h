#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZB_PERSIST_VERSION 1U

typedef struct {
    uint32_t version;
    bool has_network;
    uint8_t channel;
    uint16_t pan_id;
    uint16_t short_addr;
    uint8_t ext_pan_id[8];
    uint8_t network_key_seq;
} zb_persist_state_t;

esp_err_t zb_persistence_init(void);
esp_err_t zb_persistence_load(zb_persist_state_t *out_state);
esp_err_t zb_persistence_save(const zb_persist_state_t *state);
esp_err_t zb_persistence_clear(void);

#ifdef __cplusplus
}
#endif
