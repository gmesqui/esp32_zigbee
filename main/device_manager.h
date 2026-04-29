#pragma once
#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Limits
// ---------------------------------------------------------------------------
#define MAX_DEVICES          32
#define MAX_ENDPOINTS         8
#define MAX_CLUSTERS_PER_EP  32
#define MAX_BINDINGS_PER_EP   8
#define MAX_REPORT_CFG_TRACKED 24
#define FRIENDLY_NAME_LEN    33   // 32 printable chars + NUL

// ---------------------------------------------------------------------------
// Binding record
// ---------------------------------------------------------------------------
typedef struct {
    uint16_t cluster_id;
    uint8_t  dst_addr_mode;
    uint16_t dst_group_addr;
    uint64_t dst_ieee_addr;
    uint8_t  dst_endpoint;
} binding_record_t;

typedef enum {
    REPORT_CFG_RESULT_PENDING = 0,
    REPORT_CFG_RESULT_OK      = 1,
    REPORT_CFG_RESULT_FAIL    = 2,
    REPORT_CFG_RESULT_MISSING = 3,
    REPORT_CFG_RESULT_BIND_FAIL = 4,
    REPORT_CFG_RESULT_WRITE_FAIL = 5,
    REPORT_CFG_RESULT_UNSUPPORTED = 6,
} report_cfg_result_t;

typedef struct {
    uint8_t  endpoint;
    uint16_t cluster_id;
    uint16_t attr_id;
    uint8_t  status;
    uint8_t  result;
} report_cfg_record_t;

// ---------------------------------------------------------------------------
// Endpoint record
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t  endpoint_id;
    uint16_t profile_id;
    uint16_t device_id;
    uint8_t  device_version;
    uint8_t  in_cluster_count;
    uint16_t in_clusters[MAX_CLUSTERS_PER_EP];
    uint8_t  out_cluster_count;
    uint16_t out_clusters[MAX_CLUSTERS_PER_EP];
    uint8_t  binding_count;
    binding_record_t bindings[MAX_BINDINGS_PER_EP];
} endpoint_record_t;

// ---------------------------------------------------------------------------
// Device state machine
// ---------------------------------------------------------------------------
typedef enum {
    DEV_STATE_NEW          = 0,  // just discovered, no data yet
    DEV_STATE_INTERVIEWING = 1,  // interview in progress
    DEV_STATE_INTERVIEWED  = 2,  // descriptors + identity known
    DEV_STATE_CONFIGURED   = 3,  // reporting configured
    DEV_STATE_FAILED       = 4,  // interview failed after retries
} device_state_t;

// ---------------------------------------------------------------------------
// Device record
// ---------------------------------------------------------------------------
typedef struct {
    // --- Identity (persisted) ---
    uint64_t  ieee_addr;                       // primary key
    uint16_t  nwk_addr;                        // mutable
    char      friendly_name[FRIENDLY_NAME_LEN];

    // --- ZDO node descriptor (persisted) ---
    uint16_t  node_desc_flags;
    uint8_t   mac_capability_flags;            // bit3=RxOnWhenIdle
    uint16_t  manufacturer_code;
    uint16_t  power_desc_flags;

    // --- ZCL Basic cluster identity (persisted) ---
    char      manufacturer[33];
    char      model[33];
    uint8_t   power_source;

    // --- Endpoints (persisted) ---
    uint8_t   endpoint_count;
    endpoint_record_t endpoints[MAX_ENDPOINTS];

    // --- Reporting flag (persisted) ---
    bool      reporting_configured;

    // --- Derived (persisted, recalculated on load) ---
    bool      is_sleepy;    // !(mac_capability_flags & 0x08)

    // --- Runtime state (NOT persisted) ---
    device_state_t state;
    bool      online;
    uint32_t  last_seen_ms;
    int8_t    last_rssi;
    uint8_t   last_lqi;
    bool      radio_metrics_valid;
    bool      report_cfg_in_progress;
    uint32_t  report_cfg_started_ms;
    uint16_t  report_cfg_expected;
    uint16_t  report_cfg_received;
    uint16_t  report_cfg_failed;
    uint8_t   report_cfg_record_count;
    bool      report_cfg_record_overflow;
    report_cfg_record_t report_cfg_records[MAX_REPORT_CFG_TRACKED];

    // --- Telemetry counters (NOT persisted) ---
    uint32_t  report_attr_ok;
    uint32_t  report_attr_unchanged;
    uint32_t  read_rsp_ok;
    uint32_t  read_rsp_fail;
    uint32_t  interview_attempts;
    uint32_t  last_probe_ms;
    bool      binding_refresh_active;
    uint16_t  slot_generation;

    // --- Internal flags ---
    bool      dirty;          // needs NVS write
    bool      in_use;         // slot occupied
} device_record_t;

// ---------------------------------------------------------------------------
// Initialise the device table (call before any other dm_ function).
// ---------------------------------------------------------------------------
void dm_init(void);

// ---------------------------------------------------------------------------
// Lookup
// ---------------------------------------------------------------------------

/** Find device by IEEE address. Returns NULL if not found. */
device_record_t *dm_find_by_ieee(uint64_t ieee);

/** Find device by friendly name. Returns NULL if not found. */
device_record_t *dm_find_by_friendly_name(const char *name);

/** Find device by NWK short address. Returns NULL if not found. */
device_record_t *dm_find_by_nwk(uint16_t nwk_addr);

/** Return device at slot index [0..MAX_DEVICES-1]. NULL if slot empty. */
device_record_t *dm_get_by_index(uint8_t idx);

/** Return device at slot index only if the current generation still matches. */
device_record_t *dm_get_by_index_generation(uint8_t idx, uint16_t generation);

/** Return number of occupied slots. */
uint8_t dm_count(void);

/** Return the current slot generation. Zero if idx is out of range. */
uint16_t dm_slot_generation(uint8_t idx);

// ---------------------------------------------------------------------------
// Create / update
// ---------------------------------------------------------------------------

/** Get existing device or allocate a new slot.
 *  If new, initialises the record with ieee+nwk and state=NEW.
 *  Returns NULL if the table is full. */
device_record_t *dm_get_or_create(uint64_t ieee, uint16_t nwk_addr);

/** Update the NWK address for an existing device (e.g. after rejoin).
 *  Marks dirty so the new short address gets persisted. */
void dm_update_nwk(device_record_t *dev, uint16_t new_nwk_addr);

/** Record a radio contact (updates last_seen, online=true).
 *  Passing lqi=0 and rssi=0 means "radio metrics unknown" and preserves the
 *  previous last_lqi/last_rssi values. */
void dm_touch(device_record_t *dev, uint8_t lqi, int8_t rssi);

/** Change online/offline state and emit availability event on transition. */
bool dm_set_online(device_record_t *dev, bool online);

/** Set friendly name. Truncated to FRIENDLY_NAME_LEN-1 chars. Marks dirty. */
void dm_set_friendly_name(device_record_t *dev, const char *name);

/** Return slot index for a device pointer, or -1 if it does not belong to the table. */
int dm_index_of(const device_record_t *dev);

/** Remove one device from the RAM table and free its slot. Returns removed index or -1. */
int dm_remove(device_record_t *dev);

// ---------------------------------------------------------------------------
// Presence timer — call every ~30 s to mark stale devices offline.
// ---------------------------------------------------------------------------
void dm_check_presence(void);

// ---------------------------------------------------------------------------
// Convenience: name for display (friendly_name if set, else IEEE string).
// Uses a static buffer — copy if you need persistence beyond the call.
// ---------------------------------------------------------------------------
const char *dm_display_name(const device_record_t *dev);

// ---------------------------------------------------------------------------
// Mutex helpers (used internally and by serial_cmd for JSON dump).
// ---------------------------------------------------------------------------
void dm_lock(void);
void dm_unlock(void);

// ---------------------------------------------------------------------------
// Endpoint/cluster helpers
// ---------------------------------------------------------------------------

/** Returns true if the device has the given cluster as an input cluster
 *  on any of its endpoints. If ep_out != NULL, writes the first endpoint
 *  that has it. */
bool dm_has_in_cluster(const device_record_t *dev, uint16_t cluster_id,
                        uint8_t *ep_out);

/** Returns true when the cached interview descriptors are complete enough
 *  to route reads, commands and reporting configuration safely. */
bool dm_has_complete_descriptors(const device_record_t *dev);

/** Clear all stored bindings for a device. */
void dm_clear_bindings(device_record_t *dev);

/** Append one binding to the matching source endpoint. Returns true on success. */
bool dm_add_binding(device_record_t *dev, uint8_t src_endpoint,
                    const binding_record_t *binding);
