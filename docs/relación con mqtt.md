---
name: Relacion con MQTT
overview: "Documentacion de como usamos MQTT: topics del bridge, estado de entidades Zigbee y formato publicado en bridge/devices."
todos: []
isProject: false
---

# Resumen: Relacion con MQTT

## Modelo general

- Prefijo: todos los topics relativos se publican bajo `mqtt.base_topic`, cuyo valor por defecto es `esp32_zigbee`.
- Suscripcion: al conectar, el cliente MQTT se suscribe a `esp32_zigbee/#`.
- Publicacion: el bridge publica su estado, eventos de red Zigbee, estados por entidad y un inventario retenido en `bridge/devices`.

## Topics que publica el bridge

### `bridge/state`

- Topic: `esp32_zigbee/bridge/state`
- Payload:

```json
{"state":"online"}
```

- El LWT publica:

```json
{"state":"offline"}
```

### `bridge/info`

- Topic: `esp32_zigbee/bridge/info`
- Contenido actual:
  - `version`
  - `network.channel`
  - `network.pan_id`
  - `permit_join`

Ejemplo:

```json
{"version":"1.0.0","network":{"channel":15,"pan_id":"0x1234"},"permit_join":false}
```

### `bridge/event`

- Topic: `esp32_zigbee/bridge/event`
- Publica eventos del ciclo de vida Zigbee:
  - `device_joined`
  - `device_leave`
  - `device_interview`
  - `permit_join`

Ejemplos:

```json
{"type":"device_joined","data":{"friendly_name":"sensor_salon","ieee_address":"0x00158D0001234567"}}
```

```json
{"type":"device_interview","data":{"friendly_name":"sensor_salon","ieee_address":"0x00158D0001234567","status":"successful","supported":true}}
```

### `bridge/devices`

- Topic: `esp32_zigbee/bridge/devices`
- Retenido: si
- Objetivo: formato lo mas cercano posible a `zigbee2mqtt/bridge/devices`.

El payload actual incluye:

- Entrada `Coordinator`.
- Un objeto por dispositivo entrevistado.
- `definition`, `exposes` y `options` cuando el dispositivo coincide con una definicion conocida de z2m que tengamos implementada.
- Campos base compatibles con z2m:
  - `disabled`
  - `endpoints`
  - `friendly_name`
  - `ieee_address`
  - `interview_completed`
  - `interview_state`
  - `interviewing`
  - `manufacturer`
  - `model_id`
  - `network_address`
  - `power_source`
  - `supported`
  - `type`

Cada `endpoint` se publica con forma z2m:

- `bindings`
- `clusters.input`
- `clusters.output`
- `configured_reportings`
- `scenes`

Ejemplo resumido:

```json
[
  {
    "friendly_name": "Coordinator",
    "ieee_address": "0x00124b0024c1f36f",
    "interview_completed": true,
    "interview_state": "SUCCESSFUL",
    "interviewing": false,
    "network_address": 0,
    "supported": true,
    "type": "Coordinator",
    "disabled": false,
    "endpoints": {
      "1": {
        "bindings": [],
        "clusters": {
          "input": [],
          "output": []
        },
        "configured_reportings": [],
        "scenes": []
      }
    }
  },
  {
    "friendly_name": "temperatura_salon",
    "ieee_address": "0x00124B002935E897",
    "manufacturer": "eWeLink",
    "model_id": "TH01",
    "network_address": 61961,
    "power_source": "Battery",
    "interview_completed": true,
    "interview_state": "SUCCESSFUL",
    "interviewing": false,
    "supported": true,
    "type": "EndDevice",
    "disabled": false,
    "endpoints": {
      "1": {
        "bindings": [],
        "clusters": {
          "input": [
            "genBasic",
            "msTemperatureMeasurement",
            "msRelativeHumidity",
            "genPowerCfg"
          ],
          "output": [
            "genIdentify"
          ]
        },
        "configured_reportings": [
          {
            "attribute": "measuredValue",
            "cluster": "msTemperatureMeasurement",
            "maximum_report_interval": 3600,
            "minimum_report_interval": 10,
            "reportable_change": 10
          }
        ],
        "scenes": []
      }
    }
  }
]
```

Limitaciones actuales frente a zigbee2mqtt:

- El `Coordinator` usa endpoints minimos, no una descripcion completa del coordinador real.
- `definition/exposes/options` solo se publica para el subconjunto de modelos que ya tenemos alineados con z2m.
- Si `bridge/devices` no cabe en el buffer fijo actual, el bridge no publica ese retained en vez de enviar JSON truncado.
- No publicamos aun:
  - `date_code`
  - `software_build_id`
- `configured_reportings` refleja la configuracion que aplica nuestro bridge, no una lectura exhaustiva del dispositivo.
- Los `bindings` dependen de la respuesta real a `BindingTable_rsp`; si el dispositivo no responde todavia, se mantiene el ultimo valor conocido en cache.

### Estado por entidad

- Topic: `esp32_zigbee/<friendly_name>`
- Retenido: si
- Contenido:
  - atributos ZCL ya cacheados
  - `linkquality` cuando esta disponible
  - `last_seen`

Ejemplo:

```json
{"temperature":21.50,"humidity":45.20,"linkquality":120,"last_seen":12345.678}
```

### Disponibilidad por entidad

- Topic: `esp32_zigbee/<friendly_name>/availability`
- Payload:

```json
{"state":"online"}
```

o

```json
{"state":"offline"}
```

## Mensajes entrantes

Actualmente implementados:

- `esp32_zigbee/bridge/request/health_check`
- `esp32_zigbee/bridge/request/permit_join`
- `esp32_zigbee/bridge/request/device/rename`
- `esp32_zigbee/bridge/request/device/interview`
- `esp32_zigbee/bridge/request/device/configure`

Todos responden en:

- `esp32_zigbee/bridge/response/<clave>`

Formato de respuesta compatible con z2m:

- `{"data":{...},"status":"ok"}`
- `{"data":{},"error":"...","status":"error"}`
- Si la peticion incluye `transaction`, se reenvia en la respuesta.

### Control de entidades

- Topic: `esp32_zigbee/<nombre>/set`
- Topic: `esp32_zigbee/<nombre>/set/<atributo>`

Estado actual:

- Implementado parcialmente.
- Soportado hoy en `/set`:
  - `state`
  - `brightness`
- Soportado hoy en `/get`:
  - `state`
  - `brightness`
  - `temperature`
  - `humidity`
  - `pressure`
  - `illuminance`
  - `occupancy`
  - `battery`
  - `voltage`
  - `power`
  - `contact`
  - `tamper`
  - `battery_low`
- Si el dispositivo no expone el cluster correspondiente, la orden se ignora y solo queda reflejada en logs.

Ejemplos:

```json
{"state":"ON"}
```

o bien valor simple en subtopic.

Ejemplos soportados:

```json
{"state":"ON","brightness":180}
```

para `esp32_zigbee/luz_salon/set`.

Payload simple:

```text
ON
```

para `esp32_zigbee/luz_salon/set/state`.

Payload simple:

```text
180
```

para `esp32_zigbee/luz_salon/set/brightness`.

Lectura:

```json
{"state":"","brightness":"","power":""}
```

para `esp32_zigbee/luz_salon/get`.

Lectura por subtopic:

- payload vacio para `esp32_zigbee/luz_salon/get/state`.

### API del bridge

- Topic: `esp32_zigbee/bridge/request/<clave>`

Ejemplo:

```json
{"value":true,"time":254}
```

para `esp32_zigbee/bridge/request/permit_join`.

Ejemplos soportados:

```json
{"time":254}
```

para `esp32_zigbee/bridge/request/permit_join`.

```json
{"from":"sensor_salon","to":"sensor/cocina"}
```

para `esp32_zigbee/bridge/request/device/rename`.

```json
{"id":"sensor_salon"}
```

para `esp32_zigbee/bridge/request/device/interview` y
`esp32_zigbee/bridge/request/device/configure`.

## Resumen Zigbee -> MQTT

- Eventos de red Zigbee -> `bridge/event`
- Inventario retenido del bridge -> `bridge/devices`
- Estado agregado de cada entidad -> `esp32_zigbee/<friendly_name>`
- Disponibilidad -> `esp32_zigbee/<friendly_name>/availability`
