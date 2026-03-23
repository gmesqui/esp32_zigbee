#include "zb_trace.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "timebase.h"

static const char *TAG = "zb_trace";
static uint32_t s_trace_msg_id = 0;

typedef struct {
    uint16_t id;
    const char *name;
} id_name_pair_t;

static const id_name_pair_t s_profile_names[] = {
    {0x0000, "PROFILE_ZDO"},
    {0x0104, "PROFILE_HA"},
    {0xC05E, "PROFILE_ZLL"},
};

static const id_name_pair_t s_cluster_names[] = {
    {0x0000, "CL_BASIC"},
    {0x0003, "CL_IDENTIFY"},
    {0x0004, "CL_GROUPS"},
    {0x0005, "CL_SCENES"},
    {0x0006, "CL_ON_OFF"},
    {0x0008, "CL_LEVEL"},
    {0x0402, "CL_TEMP"},
    {0x0405, "CL_HUMIDITY"},
    {0x0500, "CL_IAS_ZONE"},
    {0x8004, "ZDO_SIMPLE_DESC_RSP"},
    {0x8031, "ZDO_MGMT_LQI_RSP"},
};

static const char *name_or_hex(const id_name_pair_t *table, size_t table_len, uint16_t id, char *tmp, size_t tmp_len)
{
    for (size_t i = 0; i < table_len; ++i) {
        if (table[i].id == id) {
            return table[i].name;
        }
    }
    snprintf(tmp, tmp_len, "0x%04X", id);
    return tmp;
}

static void bytes_to_hex(const uint8_t *data, size_t len, char *out, size_t out_len)
{
    if (out_len == 0) {
        return;
    }
    out[0] = '\0';
    size_t w = 0;
    for (size_t i = 0; i < len; ++i) {
        int n = snprintf(out + w, out_len - w, "%02X%s", data[i], (i + 1U < len) ? " " : "");
        if (n <= 0) {
            break;
        }
        w += (size_t)n;
        if (w >= out_len) {
            out[out_len - 1U] = '\0';
            break;
        }
    }
}

uint32_t zb_trace_log_packet(zb_trace_dir_t dir, const zb_trace_meta_t *meta, const uint8_t *payload, size_t len)
{
    if (meta == NULL || payload == NULL) {
        return 0;
    }

    char profile_tmp[12];
    char cluster_tmp[12];
    char raw_buf[256];
    const char *profile_name = name_or_hex(s_profile_names, sizeof(s_profile_names) / sizeof(s_profile_names[0]), meta->profile_id,
                                           profile_tmp, sizeof(profile_tmp));
    const char *cluster_name =
        name_or_hex(s_cluster_names, sizeof(s_cluster_names) / sizeof(s_cluster_names[0]), meta->cluster_id, cluster_tmp, sizeof(cluster_tmp));
    const char *dir_s = (dir == ZB_DIR_RX) ? "RX" : "TX";
    const uint32_t msg_id = ++s_trace_msg_id;

    bytes_to_hex(payload, len, raw_buf, sizeof(raw_buf));
    ESP_LOGI(TAG, "[T+%07.3f] %s#%06lu RAW aps[nwk_seq=0x%02X,aps_ctr=0x%02X] %s", timebase_now_s(), dir_s, (unsigned long)msg_id,
             meta->nwk_seq, meta->aps_counter,
             raw_buf);

    ESP_LOGI(TAG,
             "[T+%07.3f] %s#%06lu DECODE src=0x%04X/%u dst=0x%04X/%u profile=%s cluster=%s rssi=%d lqi=%u "
             "fields{fcf,seq,cmd_id,...}",
             timebase_now_s(), dir_s, (unsigned long)msg_id, meta->src_short, meta->src_ep, meta->dst_short, meta->dst_ep, profile_name,
             cluster_name, meta->rssi,
             meta->lqi);

    ESP_LOGI(TAG, "[T+%07.3f] %s#%06lu IMPACT Mensaje %s procesado, evaluar callbacks de red/aplicacion", timebase_now_s(), dir_s,
             (unsigned long)msg_id, cluster_name);
    return msg_id;
}
