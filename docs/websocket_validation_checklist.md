# WebSocket validation checklist

Use this checklist to validate the ESP32 WebSocket transport without Homebridge.

## Firmware console

1. Open the ESP32 serial monitor.
2. Press `?` and verify the help lists `w - WebSocket protocol self-test`.
3. Press `w`.
4. Confirm the summary prints `failed=0`.
5. If `session lifecycle` is skipped, disconnect any active WebSocket client and run `w` again.

## Generic WebSocket client

1. Connect a generic client to:

   ```text
   ws://<gateway-ip>:8080/ws
   ```

2. Confirm the ESP32 logs:

   ```text
   WS client connected
   WS autonomous sync: begin
   WS autonomous sync: sending fragmented inventory
   WS autonomous sync: sending initial state snapshot
   WS stream mode: active
   ```

3. Confirm the client immediately receives:

   ```text
   hello_ack
   inventory_chunk
   state_chunk
   ```

4. Check that the final inventory and state chunks contain:

   ```text
   "final":true
   ```

5. Trigger a Zigbee change:

   - toggle a paired switch
   - change a sensor value
   - open/close permit join with the serial command `j`
   - rename a device with the serial command `n`

6. Confirm the client receives the expected stream message:

   - attribute change: `event` with `changes`
   - availability change: `event` with `reachable`
   - permit join: `event` with `permit_join`
   - rename/model update: `device_updated` followed by refreshed chunks

7. Disconnect the WebSocket client.
8. Reconnect it.
9. Confirm the ESP32 starts autonomous sync again and emits a fresh `hello_ack`,
   `inventory_chunk` stream, and `state_chunk` stream.

