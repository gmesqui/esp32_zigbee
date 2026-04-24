# ESP32-C5 / ESP32-C6 Zigbee Coordinator

Coordinador Zigbee completo para `ESP32-C5-KITC-A V1.2` y `ESP32-C6 Super Mini` con `ESP-IDF` y `ESP Zigbee SDK`.

## Hardware soportado

| Placa | Target ESP-IDF | Radio | Flash | LED RGB | Boton permit-join |
|-------|----------------|-------|-------|---------|-------------------|
| `ESP32-C5-KITC-A V1.2` | `esp32c5` | IEEE 802.15.4 nativa 2.4 GHz | 4 MiB | WS2812 GPIO 27 | GPIO 28 |
| `ESP32-C6 Super Mini` | `esp32c6` | IEEE 802.15.4 nativa 2.4 GHz | 4 MiB | WS2812 GPIO 8 | GPIO 9 |

- Ethernet: W5500 por SPI (`SPI2_HOST`)
- Consola: UART0 115200 baud en C5; USB Serial/JTAG nativo por USB-C en C6 Super Mini.
- La seleccion de pines se hace en `main/board_config.h` usando `CONFIG_IDF_TARGET`.

---

## Transporte WebSocket

El transporte operativo para clientes externos es WebSocket sobre ESP-IDF:

- URL: `ws://<gateway>:8080/ws`
- Modulo servidor: `ws_transport.c/h`
- Protocolo y parsing: `ws_protocol.c/h`
- Adaptacion del modelo interno: `ws_model.c/h`
- Sesion activa: `ws_client_session.c/h`
- Puente de productores Zigbee/modelo: `client_events.c/h`
- Self-test por UART: tecla `w`

Al conectarse un cliente, el ESP32 inicia el stream autonomo sin esperar un
mensaje entrante: envia `hello_ack`, inventario fragmentado, estado inicial y
despues queda en modo stream para eventos futuros.

---

## Arquitectura

```
app_main()
  ├─ nvs_flash_init()
  ├─ dm_init()              ← tabla RAM de dispositivos
  ├─ nvs_cache_load()       ← restaura registros desde NVS
  ├─ zcl_handler_init()     ← cache de atributos
  ├─ di_init()              ← maquina de estados de entrevista
  ├─ led_driver_init()      ← tarea LED @ 50 Hz
  ├─ button_handler_init()  ← ISR boton BOOT
  ├─ serial_cmd_init()      ← tarea UART0
  └─ zigbee_core_init()     ← tarea Zigbee + main loop (nunca retorna)
```

### Tareas FreeRTOS

| Tarea | Prioridad | Proposito |
|-------|-----------|-----------|
| `zigbee_main` | 5 | Loop principal del stack Zigbee |
| `btn_task` | 3 | Procesador de eventos de boton |
| `led_task` | 2 | Animacion LED @ 50 Hz |
| `serial_cmd_task` | 1 | Listener consola activa (`UART0` en C5, `USB Serial/JTAG` en C6) |

### Ficheros fuente

| Fichero | Responsabilidad |
|---------|----------------|
| `main.c` | Punto de entrada, secuencia de inicializacion |
| `board_config.h` | Seleccion de placa, pines de LED/boton/Ethernet y nombre de hardware por target ESP-IDF |
| `zigbee_core.c/h` | Init coordinador, manejador de senales BDB/ZDO, manejador de acciones ZCL, alarmas de mantenimiento |
| `device_manager.c/h` | Tabla RAM thread-safe (max 32 dispositivos), ciclo de vida, lookups por IEEE/NWK |
| `device_interview.c/h` | Maquina de estados de entrevista (7 pasos), cola FIFO, callbacks ZDO, resolucion IEEE |
| `zcl_handler.c/h` | Decodificacion de Report Attributes y Read Attribute Responses, cache con deteccion de cambios |
| `report_config.c/h` | Envio de Configure Reporting tras entrevista (13 clusters), respuesta IAS enrollment |
| `nvs_cache.c/h` | Serializacion/deserializacion binaria de dispositivos en NVS, escrituras lazy por bandera dirty |
| `serial_cmd.c/h` | Comandos interactivos por consola activa (teclas 1–5, g, w, n, j, r, e, ?) |
| `led_driver.c/h` | Control WS2812: onda seno verde-azul base, overlay rojo permit-join, overlay blanco actividad |
| `button_handler.c/h` | ISR con antirebote 200 ms, toggle permit-join 180 s |
| `utils.c/h` | Uptime ms/s, IEEE→string, nombres de cluster/device-type, macro `ZB_LOG` con timestamp |

---

## Funcionalidades implementadas

### Coordinador Zigbee

- Perfil Home Automation (0x0104), dispositivo Home Gateway (0x0050)
- Radio nativa, sin modo host
- Max 20 hijos (routers + end devices)
- Endpoint 1 con clusters Basic e Identify
- Mascara de canales: todos los canales 2.4 GHz
- Persistencia de red en NVS (namespace Zigbee stack): canal, PAN ID, ext PAN ID, clave de red

### Tabla de dispositivos en RAM

- 32 slots, protegida con mutex
- Estado por dispositivo: `NEW` → `INTERVIEWING` → `INTERVIEWED` → `CONFIGURED` → `FAILED`
- Campos por dispositivo:
  - `ieee`, `nwk_addr`, `friendly_name`
  - `online`, `is_sleepy`
  - `manufacturer`, `model`, `power_source`
  - `node_desc_flags`, `mac_capability_flags`, `manufacturer_code`, `power_desc_flags`
  - Hasta 8 endpoints, cada uno con `profile_id`, `device_id`, `clusters_in[]`, `clusters_out[]`
  - `last_seen_ms`, `lqi`, `rssi`
  - `reporting_configured`
  - Lecturas: `temperature_c`, `humidity_pct`, `on_off`, `occupancy`, `illuminance_raw`, `illuminance_lux`, `pressure_kpa`, `ias_zone_status`, `battery_mv`, `battery_pct`, `level`, `power_watts`
  - Contadores: `report_attr_ok`, `report_attr_unchanged`, `read_rsp_ok`, `read_rsp_fail`, `interview_attempts`

### Entrevista automatica (7 pasos)

1. Node Descriptor Request (ZDO)
2. Power Descriptor Request (ZDO)
3. Active Endpoints Request (ZDO)
4. Simple Descriptor Request por endpoint (ZDO)
5. Read Basic Cluster (0x0000, attrs: fabricante, modelo, fuente de alimentacion)
6. Read Power Config (0x0001, attrs: tension y porcentaje de bateria)
7. Configure Reporting (13 clusters estandar)

- Reintentos: 3 con backoff; marca FAILED si se agotan
- Cola FIFO de 8 posiciones para joins simultaneos
- Resolucion asıncrona de IEEE Address si se desconoce la direccion corta

### Clusters soportados para reporte / lectura

| Cluster | Nombre | Atributos relevantes |
|---------|--------|----------------------|
| 0x0001 | POWER_CFG | Tension bateria (0x0020), porcentaje (0x0021) |
| 0x0006 | ON_OFF | Estado encendido (0x0000) |
| 0x0008 | LEVEL | Nivel (0x0000, 0–254) |
| 0x0300 | COLOR_CTRL | Hue, Saturation, Color Temperature |
| 0x0400 | ILLUMINANCE | Iluminancia raw + conversion log a lux |
| 0x0402 | TEMPERATURE | Temperatura int16 ÷ 100 → °C |
| 0x0403 | PRESSURE | Presion int16 ÷ 10 → kPa |
| 0x0405 | HUMIDITY | Humedad uint16 ÷ 100 → % |
| 0x0406 | OCCUPANCY | Bitmap presencia |
| 0x0500 | IAS_ZONE | Estado zona alarma + enrollment |
| 0x0B04 | ELEC_MEAS | Potencia activa (0x050B) |

### Configure Reporting enviado tras entrevista

| Cluster | Atributo | Tipo | Min (s) | Max (s) | Threshold |
|---------|----------|------|---------|---------|-----------|
| 0x0006 | 0x0000 | bool | 0 | 3600 | — |
| 0x0008 | 0x0000 | uint8 | 1 | 3600 | 1 |
| 0x0300 | 0x0000, 0x0001 | uint8 | 1 | 3600 | 1 |
| 0x0300 | 0x0007 | uint16 | 1 | 3600 | 10 |
| 0x0402 | 0x0000 | int16 | 10 | 3600 | 10 |
| 0x0405 | 0x0000 | uint16 | 10 | 3600 | 50 |
| 0x0403 | 0x0000 | int16 | 10 | 3600 | 10 |
| 0x0400 | 0x0000 | uint16 | 10 | 3600 | 500 |
| 0x0406 | 0x0000 | bitmap8 | 0 | 3600 | — |
| 0x0001 | 0x0021 | uint8 | 3600 | 43200 | 2 |
| 0x0500 | 0x0002 | bitmap16 | 0 | 3600 | — |
| 0x0B04 | 0x050B | int16 | 5 | 3600 | 10 |

### Deteccion de presencia (offline)

- **Siempre encendido**: timeout 7260 s (~2 h) = 2 × max_interval + margen
- **Sleepy (bateria)**: timeout 10920 s (~3 h) = 3 × max_interval + margen
- Verificacion cada 10 s via alarma de mantenimiento

### Persistencia NVS de dispositivos

- Namespace: `zb_cache`
- Claves: `dt3_head` (version + count), `dt3_d00`–`dt3_d31` (blobs por dispositivo)
- Version de esquema: **3** (los blobs de versiones anteriores se rechazan)
- Escritura lazy: solo los registros con bandera `dirty` se escriben; se limpia tras commit
- Periodicidad: cada 10 s via alarma de mantenimiento (solo si hay cambios)
- Al arrancar: los dispositivos restaurados vuelven con estado INTERVIEWED, `online=false`
- `last_seen` y cambios de RSSI/LQI NO disparan escritura NVS

> Si ves `ESP_ERR_NVS_NOT_ENOUGH_SPACE` al guardar `dt3_d31`, no reduzcas la particion `nvs` (actualmente 128 KiB).

### LED WS2812

| Estado | Patron |
|--------|--------|
| Normal | Onda seno verde-azul, periodo 4 s |
| Permit-join abierto | Overlay rojo pulsante, periodo 1 s |
| Actividad ZCL | Flash blanco 100 ms con decay |

### Boton BOOT

- Pulsacion corta: abre permit-join 180 s (o cierra si ya estaba abierto)
- Antirebote hardware: 200 ms

### Patillaje Ethernet (W5500 por SPI)

| Senal | ESP32-C5 GPIO | ESP32-C6 GPIO | Notas |
|-------|---------------|---------------|-------|
| `MOSI` | 4 | 2 | SPI hacia el W5500 |
| `MISO` | 5 | 3 | SPI desde el W5500 |
| `SCLK` | 6 | 6 | Reloj SPI |
| `CS` | 23 | 7 | Chip select del W5500 |
| `INT` | 24 | 18 | Interrupcion del W5500 |
| `RST` | 25 | 14 | Reset del W5500 |

- Host SPI usado: `SPI2_HOST`
- Frecuencia SPI configurada: `20 MHz`

---

## Comandos serie

Canal interactivo segun target:

- `ESP32-C5`: `UART0` a `115200`
- `ESP32-C6`: `USB Serial/JTAG` nativo por USB-C

| Tecla | Accion |
|-------|--------|
| `1` | Emite JSON completo con tabla de dispositivos |
| `2` | Estadisticas de red (canal, PAN ID, ext PAN ID, IEEE coordinador, conteo online) |
| `3` | Lista de tareas FreeRTOS (nombre, estado, prioridad, stack) |
| `4` | Estadisticas de heap (libre actual, minimo historico) |
| `5` | Estado de la cola de entrevista |
| `n` | Asignar nombre amigable a un dispositivo (pide IEEE y nombre) |
| `j` | Toggle permit-join (abre 180 s o cierra) |
| `r` | Re-entrevistar dispositivo (pide IEEE, resetea estado a NEW) |
| `e` | Borrar cache NVS (pide confirmacion "YES") |
| `w` | Ejecutar self-test del protocolo WebSocket |
| `?` | Ayuda: imprime mapa de teclas |

---

## Formato de log

Todos los logs usan timestamp relativo al arranque:

```
[T+%07.3f]  →  [T+012.345]
```

Por cada mensaje ZCL/ZDO se emiten hasta 3 lineas:

1. `RAW` — bytes en hexadecimal y metadatos APS/NWK
2. `DECODE` — campos interpretados (profile, cluster, seq, etc.)
3. `IMPACT` — significado del mensaje y evento local desencadenado

---

## Salida JSON (tecla `1`)

```json
{
  "ts_s": 123.456,
  "device_count": 2,
  "devices": [
    {
      "ieee": "0x00124B00AABBCCDD",
      "friendly_name": "Sensor salon",
      "short": "0x1234",
      "online": true,
      "is_sleepy": false,
      "manufacturer": "Sonoff",
      "model": "SNZB-02",
      "power_source": "battery",
      "state": "configured",
      "last_seen_s": 120.123,
      "lqi": 189,
      "rssi": -56,
      "reporting_configured": true,
      "endpoints": [
        {
          "id": 1,
          "profile": "0x0104",
          "device_id": "TEMP_SENSOR",
          "in_clusters": ["0x0000", "0x0001", "0x0003", "0x0402", "0x0405"],
          "out_clusters": ["0x0019"]
        }
      ],
      "readings": {
        "temperature_c": 21.45,
        "humidity_pct": 58.12,
        "battery_mv": 3000,
        "battery_pct": 85
      },
      "stats": {
        "report_attr_ok": 42,
        "report_attr_unchanged": 18,
        "read_rsp_ok": 7,
        "read_rsp_fail": 1,
        "interview_attempts": 1
      }
    }
  ]
}
```

Solo se incluyen en `readings` las claves que tienen dato (no se emiten `null`).
Si la iluminancia raw es 0 (demasiado baja), no se incluye `illuminance_lux`.

Comportamiento ante informes repetidos: `last_seen_s` se actualiza siempre; `report_attr_ok` y los logs de valor solo se emiten si el dato **cambia** respecto al almacenado; `report_attr_unchanged` cuenta los que no alteraron el dato.

---

## Limites y constantes clave

| Constante | Valor | Notas |
|-----------|-------|-------|
| `MAX_DEVICES` | 32 | Slots en tabla RAM |
| `MAX_ENDPOINTS` | 8 | Por dispositivo |
| `MAX_CLUSTERS_PER_EP` | 32 | Entrada + salida combinados |
| `FRIENDLY_NAME_LEN` | 33 | 32 chars + NUL |
| `MAX_ATTR_CACHE` | 128 | Entradas cache de atributos |
| `MAX_PENDING_ATTRS` | 8 | Buffer para direcciones desconocidas |
| `IQUEUE_SIZE` | 8 | Cola FIFO de entrevista |
| `MAX_CHILDREN` | 20 | Routers + end devices |
| `MAINTENANCE_PERIOD_MS` | 10 000 | NVS flush + deteccion presencia |
| `OFFLINE_THRESHOLD_ALWAYS_ON_MS` | 7 260 000 | ~2 h |
| `OFFLINE_THRESHOLD_SLEEPY_MS` | 10 920 000 | ~3 h |
| `PERMIT_JOIN_SECS` | 180 | Ventana abierta por boton |
| `DEBOUNCE_MS` | 200 | Antirebote boton |
| `LED_STRIP_GPIO` | C5: 27, C6: 8 | GPIO WS2812 |
| `BOOT_BUTTON_GPIO` | C5: 28, C6: 9 | GPIO boton BOOT |
| `ETH_MOSI_GPIO` | C5: 4, C6: 2 | SPI MOSI hacia W5500 |
| `ETH_MISO_GPIO` | C5: 5, C6: 3 | SPI MISO desde W5500 |
| `ETH_SCLK_GPIO` | C5: 6, C6: 6 | SPI clock W5500 |
| `ETH_CS_GPIO` | C5: 23, C6: 7 | Chip select W5500 |
| `ETH_INT_GPIO` | C5: 24, C6: 18 | Interrupcion W5500 |
| `ETH_RST_GPIO` | C5: 25, C6: 14 | Reset W5500 |
| `ETH_SPI_CLOCK_HZ` | 20 000 000 | SPI Ethernet a 20 MHz |
| `NVS_CACHE_VERSION` | 3 | Version de esquema NVS |

---

## Particiones

```
nvs,        data, nvs,     0x9000,   0x20000    # 128 KiB — NVS (cache + stack Zigbee)
phy_init,   data, phy,     0x29000,  0x1000     #   4 KiB — calibracion PHY
zb_storage, data, fat,     0x2A000,  0x10000    #  64 KiB — almacenamiento persistente Zigbee
zb_fct,     data, fat,     0x3A000,  0x1000     #   4 KiB — factory data Zigbee
factory,    app,  factory, 0x40000,  0x3C0000   # 3.75 MiB — firmware
```

> Si cambias los offsets de `zb_storage` / `zb_fct`, hay que flashear la tabla de particiones y puede ser necesario volver a formar la red.

---

## Activar entorno ESP-IDF

En PowerShell:

```powershell
. "C:\Espressif\tools\Microsoft.v5.5.3.PowerShell_profile.ps1"
```

## Compilar

Para ESP32-C5:

```powershell
idf.py set-target esp32c5
idf.py build
```

Para ESP32-C6:

```powershell
idf.py set-target esp32c6
idf.py build
```

`sdkconfig.defaults` contiene la configuracion comun, y `sdkconfig.defaults.esp32c5` / `sdkconfig.defaults.esp32c6` contienen los ajustes especificos de cada target.

## Flashear y monitorizar

```powershell
.\flash-monitor.ps1
```

Solo monitorizar:

```powershell
.\monitor.ps1
```

Flashear con argumentos adicionales:

```powershell
.\idf-with-com7.ps1 flash monitor
```

En ESP32-C6 Super Mini, usa el puerto COM del USB-C nativo de la placa; `sdkconfig.defaults.esp32c6` deja la consola en USB Serial/JTAG.

Salir del monitor: `Ctrl+]`

---

## Plugin Homebridge

El plugin de Homebridge vive en una carpeta separada del firmware:

- [homebridge-plugin/README.md](/c:/projects/esp32_zigbee3/homebridge-plugin/README.md)

Se apoya en el protocolo WebSocket documentado en:

- [docs/protocolo_websocket_esp32_homebridge_v2.docx](/c:/projects/esp32_zigbee3/docs/protocolo_websocket_esp32_homebridge_v2.docx)

Objetivo del paquete:

- conectar Homebridge como cliente WebSocket al ESP32
- descubrir accesorios desde `inventory_chunk`
- mantener estado y reachability desde `state_chunk` y `event`
- traducir comandos HomeKit hacia `cmd`

El plugin se ha separado a nivel de repo para no mezclar dependencias Node/Homebridge
con el firmware ESP-IDF.
