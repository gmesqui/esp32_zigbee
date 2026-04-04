#pragma once

#include <stdbool.h>
#include "device_manager.h"

/** Returns true if the device matches a known zigbee2mqtt-style definition. */
bool dd_has_definition(const device_record_t *dev);

/** Returns true if the device should be considered supported by our native definition layer. */
bool dd_is_supported(const device_record_t *dev);

/** Appends ,"definition":{...} to the provided JSON buffer when a definition is available. */
void dd_append_definition_json(char **p, char *end, const device_record_t *dev);
