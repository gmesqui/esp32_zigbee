#pragma once
#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// NVS-based persistence for the device table.
//
// Layout in NVS namespace "zb_cache":
//   "dt3_head"  -> { uint8_t version; uint8_t count; }
//   "dt3_d00"   -> serialised blob for device 0
//   ...
//   "dt3_d31"   -> serialised blob for device 31
//
// Only slots with in_use == true are written.
// Blob format is internal — version byte guards against schema changes.
// ---------------------------------------------------------------------------

#define NVS_NAMESPACE       "zb_cache"
#define NVS_KEY_HEAD        "dt3_head"
#define NVS_KEY_DEV_PREFIX  "dt3_d"
#define NVS_CACHE_VERSION   4u   // bump on incompatible layout changes

// ---------------------------------------------------------------------------
// Load the device table from NVS into the device_manager.
// Must be called after dm_init() and nvs_flash_init().
// Returns true if valid data was found.
// ---------------------------------------------------------------------------
bool nvs_cache_load(void);

// ---------------------------------------------------------------------------
// Save all dirty devices to NVS. Call periodically (e.g. every 10 s).
// Only writes entries whose dirty flag is set; clears the flag after write.
// ---------------------------------------------------------------------------
void nvs_cache_save_dirty(void);

// ---------------------------------------------------------------------------
// Force-save a single device immediately (e.g. after interview completes).
// ---------------------------------------------------------------------------
void nvs_cache_save_device(uint8_t slot_idx);

// ---------------------------------------------------------------------------
// Erase all device cache from NVS (factory reset of device list).
// ---------------------------------------------------------------------------
void nvs_cache_erase(void);
