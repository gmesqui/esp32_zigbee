# Homebridge ESP32 Zigbee

Plugin de Homebridge para el gateway Zigbee sobre ESP32 de este repo.

La idea es mantenerlo totalmente separado del firmware ESP-IDF:

- firmware Zigbee y servidor WebSocket en la raíz del repo
- simulador Flutter en `tools/homebridge_simulator`
- plugin real de Homebridge en `homebridge-plugin`

## Estado actual

El plugin sigue el contrato descrito en
[docs/protocolo_websocket_esp32_homebridge_v2.docx](/c:/projects/esp32_zigbee3/docs/protocolo_websocket_esp32_homebridge_v2.docx)
y el comportamiento real implementado en `main/ws_protocol.c`.

Capacidades ya cubiertas:

- conexión persistente al WebSocket del ESP32
- reconstrucción del inventario desde `inventory_chunk`
- aplicación de `event` y metadatos de reachability
- reconexión automática y `resync` cuando hay cambios estructurales
- control `on/off` para dispositivos con capacidad `switch`
- exposición en HomeKit de sensores de temperatura, humedad, luz, ocupación, contacto y batería

Limitaciones importantes del firmware actual:

- `state_chunk` todavía llega con `state: {}` vacío, así que el plugin arranca apoyándose en caché y en eventos posteriores
- por WebSocket hoy solo está implementado el comando `set` sobre `cluster: "onoff"`
- capacidades como `brightness`, `pressure_sensor` o `power_sensor` todavía no tienen traducción HomeKit o no son escribibles por este protocolo

## Instalación local

```powershell
cd homebridge-plugin
npm install
npm run build
```

Luego puedes instalarlo en tu entorno Homebridge con `npm link` o empaquetarlo con
`npm pack`.

## Configuración de Homebridge

Ejemplo mínimo:

```json
{
  "platforms": [
    {
      "platform": "ESP32ZigbeeBridge",
      "name": "ESP32 Zigbee Bridge",
      "url": "ws://esp32-zigbee.local:8080/ws"
    }
  ]
}
```

Configuración equivalente por partes:

```json
{
  "platforms": [
    {
      "platform": "ESP32ZigbeeBridge",
      "name": "ESP32 Zigbee Bridge",
      "host": "esp32-zigbee.local",
      "port": 8080,
      "path": "/ws",
      "tls": false
    }
  ]
}
```

## Cómo modela accesorios

Un dispositivo Zigbee se convierte en un accesorio Homebridge si al menos tiene
una capacidad con traducción útil a HomeKit:

- `switch`
- `temperature_sensor`
- `humidity_sensor`
- `illuminance_sensor`
- `occupancy_sensor`
- `ias_zone_sensor`

`battery_sensor` se publica como servicio adicional cuando el accesorio ya tiene
otro servicio principal.

## Siguientes pasos recomendados

- completar `state_chunk` en el firmware para que Homebridge arranque con snapshot real
- exponer `brightness` y otros comandos en `cmd`
- decidir cómo publicar `pressure_sensor` y `power_sensor` en HomeKit
