# ESP32-C5 Zigbee Coordinator Base

Proyecto base para `ESP32-C5-KITC-A V1.2` con `ESP-IDF` y `ESP Zigbee SDK`.

## Objetivo

Coordinador Zigbee basico para pruebas y aprendizaje con:

- persistencia minima para recuperar red tras reinicio,
- trazado detallado de mensajes Zigbee,
- tabla en RAM de dispositivos con salida JSON por monitor serie.

## Estado actual del firmware base

El firmware implementa:

- reloj relativo desde arranque (milisegundos internos, presentacion en segundos con 3 decimales),
- almacenamiento NVS del estado minimo de red,
- traza estructurada de mensajes RX/TX con `RAW`, `DECODE` e `IMPACT`,
- tabla en RAM de dispositivos observados,
- comando por monitor: tecla `1` imprime JSON con la tabla.

## Formato de timestamp

Todos los logs usan:

- `[T+%07.3f]`

Ejemplo:

- `[T+012.345]`

## Formato de trazas Zigbee

Por cada mensaje Zigbee se emiten 3 lineas:

1. `RAW`: bytes en hexadecimal y metadatos APS/NWK.
2. `DECODE`: campos interpretados con constantes cortas (`profile`, `cluster`, etc.).
3. `IMPACT`: explicacion corta del significado y del evento local desencadenado.

Ejemplo de estilo:

- `[T+015.250] RX RAW aps[nwk_seq=0x12,aps_ctr=0x34] 18 57 0A 00 21 2C 09`
- `[T+015.250] RX DECODE src=0x1234/1 dst=0x0000/1 profile=PROFILE_HA cluster=CL_TEMP ...`
- `[T+015.251] RX IMPACT Mensaje CL_TEMP procesado, evaluar callbacks de red/aplicacion`

## Persistencia minima (obligatoria)

Se persiste en NVS solo lo necesario para reanudar red:

- `has_network`,
- `channel`,
- `pan_id`,
- `ext_pan_id`,
- `short_addr` del coordinador,
- `network_key_seq`,
- `version` de esquema.

Si el estado existe y es valido, el arranque se hace como no-factory-new.
Si no, se forma red nueva.

## Particion NVS y cache de tabla (`zb_cache`)

La tabla de dispositivos se guarda en NVS en formato **chunked** (`dt2_head`, `dt2_d0`…`dt2_d7`) **solo cuando cambia**
(entrevista, identidad, clusters, etc.); no se hace wear por temporizador. `last_seen`/RSSI por tráfico no disparan guardado.
La particion **`nvs`** debe ser lo bastante grande (en `partitions.csv` esta a **48 KiB**); si ves
`ESP_ERR_NVS_NOT_ENOUGH_SPACE` al guardar `dt2_d7`, no reduzcas esa particion sin otra estrategia.

**Importante:** si cambias offsets de `zb_storage` / `zb_fct` en `partitions.csv`, hay que flashear la
tabla de particiones; el stack Zigbee usara la nueva region `zb_storage` (la red anterior en la
direccion antigua dejara de leerse). Tras un cambio asi puede hacer falta **volver a formar** la red.

## Persistencia opcional (sugerida)

Mantener separada de la minima. Sugerencias:

- historial simplificado de enlaces (`lqi/rssi`) por dispositivo,
- contador monotono de tramas,
- parametros operativos (`permit_join_timeout`, nivel de log).

## Tabla RAM de dispositivos y JSON por tecla `1`

La tabla mantiene, por dispositivo:

- `ieee`, `short`, `device_id`,
- `endpoints`, `clusters_in`, `clusters_out`,
- `last_seen_s`, `lqi`, `rssi` (RSSI/LQI se rellenan desde la **tabla de vecinos NWK** al recibir informes ZCL, porque el SDK no los adjunta al callback de Report Attributes; en respuestas Read Attribute se usa el RSSI del marco ZCL y el LQI del vecino si existe),
- `manufacturer`, `model`, `state_flags`,
- `readings`: temperatura (°C), humedad (%), `on_off`, ocupación (bitmap), **iluminancia** (raw ZCL + `illuminance_lux` vía fórmula log del ZCL), **presión** (raw en décimas de kPa + `pressure_kpa`), **IAS Zone** (`ias_zone_status`), **batería** desde Power Config (`power_battery_mv`, `power_battery_pct`; también se reflejan en `battery_mv` / `battery_pct` a nivel dispositivo), según informes ZCL y respuestas Read Attribute; sondeo periódico ~60 s por radio (sin escritura NVS extra). **Solo se incluyen en JSON las claves de lectura que tienen dato** (no se emiten `null`). Si hay iluminancia raw `0` (demasiado baja), no se añade `illuminance_lux`.

**Informes ZCL repetidos:** varias tramas con el mismo valor (p. ej. ON/OFF) son habituales. Cada trama sigue actualizando `last_seen_s` vía traza; el evento `DEVICE_REPORT` y los logs de valor solo se emiten si la lectura **cambia** respecto a la tabla. En telemetría JSON: `report_attr_ok` cuenta informes OK; `report_attr_unchanged` los que no alteraron el dato almacenado. `read_rsp_ok` / `read_rsp_fail` corresponden a **respuestas Read Attribute**, no a informes.

**NVS cache dispositivos:** versión de blob `CACHE_VER=5` (telemetría ampliada; cachés anteriores se rechazan hasta reentrevistar).

Al pulsar `1` en el monitor serie, se emite un JSON completo:

```json
{"ts_s":12.345,"devices":[{"ieee":"0x00000000FFFF1234","short":"0x1234","device_id":"0x0000","last_seen_s":12.345,"lqi":189,"rssi":-56,"manufacturer":"","model":"","state_flags":0,"endpoints":[],"clusters_in":[],"clusters_out":[]}]}
```

## Dependencias gestionadas

Componentes declarados en `main/idf_component.yml`:

- `espressif/led_strip`
- `espressif/esp-zboss-lib`
- `espressif/esp-zigbee-lib`

## Estructura principal

- `main/main.c`: arranque, bucle principal; NVS solo ante **cambios** (estado red y cache dispositivos).
- `main/timebase.*`: reloj relativo del sistema.
- `main/zb_persistence.*`: carga/guardado de estado de red en NVS.
- `main/zb_trace.*`: trazas de mensajes Zigbee.
- `main/device_table.*`: tabla de dispositivos en RAM y salida JSON.
- `main/serial_cmd.*`: lectura de comandos por UART (`1` => dump JSON).
- `main/zb_coordinator.*`: capa de coordinador y eventos.

## Activar entorno ESP-IDF

En PowerShell:

```powershell
. "C:\Espressif\tools\Microsoft.v5.5.3.PowerShell_profile.ps1"
```

## Compilar

```powershell
idf.py set-target esp32c5
idf.py build
```

## Flashear y monitorizar (COM3)

Desde la raiz del repo:

```powershell
.\idf-with-com3.ps1 flash monitor
```

Atajo recomendado:

```powershell
.\flash-monitor.ps1
```

Salir del monitor:

- `Ctrl+]`

