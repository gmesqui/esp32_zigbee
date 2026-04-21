#include "client_events.h"

#include "zb_events.h"
#include "utils.h"

static void on_zigbee_event(const zb_event_t *evt)
{
    if (!evt) {
        return;
    }

    switch (evt->type) {
        case ZB_EVT_DEVICE_JOINED:
            break;
        case ZB_EVT_DEVICE_LEAVE:
            break;
        case ZB_EVT_INTERVIEW:
            break;
        case ZB_EVT_ATTR_CHANGED:
            break;
        case ZB_EVT_AVAILABILITY:
            break;
        case ZB_EVT_PERMIT_JOIN:
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
