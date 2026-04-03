#pragma once
#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Limits
// ---------------------------------------------------------------------------
#define MAX_DEVICES          32
#define MAX_ENDPOINTS         8
#define MAX_CLUSTERS_PER_EP  32
#define FRIENDLY_NAME_LEN    33   // 32 printable chars + NUL

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

    // --- Telemetry counters (NOT persisted) ---
    uint32_t  report_attr_ok;
    uint32_t  report_attr_unchanged;
    uint32_t  read_rsp_ok;
    uint32_t  read_rsp_fail;
    uint32_t  interview_attempts;
    uint32_t  last_probe_ms;

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

/** Find device by NWK short address. Returns NULL if not found. */
device_record_t *dm_find_by_nwk(uint16_t nwk_addr);

/** Return device at slot index [0..MAX_DEVICES-1]. NULL if slot empty. */
device_record_t *dm_get_by_index(uint8_t idx);

/** Return number of occupied slots. */
uint8_t dm_count(void);

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
