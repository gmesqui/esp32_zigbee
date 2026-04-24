#pragma once

// ---------------------------------------------------------------------------
// Serial command handler.
//
// Reads single keystrokes from the active ESP-IDF console channel and executes
// diagnostic commands.
//
// - ESP32-C5: UART0
// - ESP32-C6: native USB Serial/JTAG
//
// Key map:
//   '1' - Plain-text dump of all devices
//   '2' - Network statistics (channel, PAN ID, counters)
//   '3' - FreeRTOS task list (stack watermarks)
//   '4' - Heap statistics
//   '5' - Interview queue status
//   'n' - Set friendly name (interactive: asks for IEEE then name)
//   'j' - Toggle permit join (same as pressing BOOT button)
//   'r' - Re-interview a device (interactive: asks for IEEE)
//   'e' - Erase NVS device cache (dangerous! asks for confirmation)
//   '?' - Print key map
// ---------------------------------------------------------------------------

/** Initialise the active console input driver and start serial_cmd_task. */
void serial_cmd_init(void);
