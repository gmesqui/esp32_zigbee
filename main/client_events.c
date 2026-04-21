#include "client_events.h"

#include "zb_events.h"
#include "utils.h"
#include "ws_transport.h"
#include <string.h>

static void on_zigbee_event(const zb_event_t *evt)
{
    if (!evt) {
        return;
    }

    switch (evt->type) {
        case ZB_EVT_DEVICE_JOINED:
            ws_transport_notify_event(evt);
            ws_transport_notify_inventory();
            break;
        case ZB_EVT_DEVICE_LEAVE:
            ws_transport_notify_event(evt);
            ws_transport_notify_inventory();
            break;
        case ZB_EVT_DEVICE_UPDATED:
            ws_transport_notify_event(evt);
            ws_transport_notify_inventory();
            break;
        case ZB_EVT_INTERVIEW:
            ws_transport_notify_event(evt);
            if (evt->interview_status &&
                strcmp(evt->interview_status, "successful") == 0) {
                ws_transport_notify_inventory();
            }
            break;
        case ZB_EVT_ATTR_CHANGED:
            ws_transport_notify_state_change(evt);
            break;
        case ZB_EVT_AVAILABILITY:
            ws_transport_notify_state_change(evt);
            break;
        case ZB_EVT_PERMIT_JOIN:
            ws_transport_notify_event(evt);
            break;
        default:
            break;
    }
}

void client_events_init(void)
{
    zb_events_register(on_zigbee_event);
    ZB_LOG("client_events: registered with Zigbee event bus");
}
