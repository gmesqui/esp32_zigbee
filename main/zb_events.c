#include "zb_events.h"
#include "utils.h"
#include <stdio.h>
#include <string.h>

static zb_event_handler_t s_handlers[ZB_EVENTS_MAX_HANDLERS];
static int                s_handler_count = 0;

void zb_events_init(void)
{
    memset(s_handlers, 0, sizeof(s_handlers));
    s_handler_count = 0;
}

void zb_events_register(zb_event_handler_t handler)
{
    if (!handler) return;
    if (s_handler_count >= ZB_EVENTS_MAX_HANDLERS) {
        ZB_LOG("zb_events: handler table full");
        return;
    }
    s_handlers[s_handler_count++] = handler;
}

void zb_events_emit(const zb_event_t *evt)
{
    if (!evt) return;
    for (int i = 0; i < s_handler_count; i++) {
        if (s_handlers[i]) {
            s_handlers[i](evt);
        }
    }
}
