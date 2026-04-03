#pragma once
#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Zigbee coordinator core.
//
// Responsibilities:
//   - Platform and stack initialisation
//   - Signal handler (BDB/ZDO network events)
//   - Action handler (ZCL messages)
//   - Main loop task (esp_zb_main_loop_iteration)
//   - Periodic maintenance: NVS flush, presence check
// ---------------------------------------------------------------------------

/** Initialise and start the Zigbee stack + main loop task.
 *  Must be called from app_main after NVS is ready. */
void zigbee_core_init(void);

/** Returns true once the coordinator is on-network and operational. */
bool zigbee_core_is_ready(void);
