#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t matter_bridge_init(void);
void matter_bridge_poll(void);
void matter_bridge_factory_reset(void);

#ifdef __cplusplus
}
#endif
