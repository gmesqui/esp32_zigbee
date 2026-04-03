#pragma once

// ---------------------------------------------------------------------------
// Serial command handler.
//
// Reads single keystrokes from UART0 and executes diagnostic commands.
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

/** Initialise UART and start the serial_cmd_task. Call once from app_main. */
void serial_cmd_init(void);
