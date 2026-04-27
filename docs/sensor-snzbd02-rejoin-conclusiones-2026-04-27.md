# Conclusiones del rejoin del sensor SNZB-02D

Fecha: 2026-04-27.

Dispositivo observado: `0xF4B3B1FFFE523696`.

Modelo identificado: `SONOFF SNZB-02D`, sensor sleepy de temperatura y humedad.

Log base: `logs/tcp-console-20260427-210114.log`.

## Resumen

La entrevista y la configuracion de reporting si llegaron a funcionar cuando se uso la direccion NWK correcta `0x2C6D`. El fallo principal observado no fue falta de tiempo por ser sleepy, sino gestion incorrecta de cambios/conflictos de direccion corta NWK: un `DEVICE_ANNCE` tardio con la direccion vieja `0x510A` sobrescribio la direccion valida y provoco intentos de configuracion contra un nodo inexistente.

## Lo que fue bien

- El sensor hizo rejoin correctamente con NWK nueva `0x2C6D`.
- La entrevista ZDO funciono:
  - `NODE_DESC` correcto, fabricante `0x1286`, sleepy.
  - `POWER_DESC` correcto.
  - `ACTIVE_EP` correcto, un endpoint.
  - `SIMPLE_DESC` correcto en endpoint `1`.
- El dispositivo reporto datos reales desde `0x2C6D`:
  - temperatura,
  - humedad,
  - bateria por porcentaje (`POWER_CFG` atributo `0x0021`).
- La configuracion de reporting funciono para:
  - `TEMPERATURE`,
  - `HUMIDITY`,
  - `POWER_CFG` atributo `0x0021`.
- Los binds correspondientes llegaron a responder OK cuando se enviaron a `0x2C6D`.
- La nueva logica de presencia para sleepy evito marcar offline de inmediato por `DEVICE_UNAVAILABLE` cuando habia `last_seen` reciente.

## Lo que fue mal

- Tras respuestas correctas por `0x2C6D`, llego un `DEVICE_ANNCE` tardio con NWK vieja `0x510A`.
- El firmware acepto ese announce como verdad y actualizo `dev->nwk_addr` a `0x510A`.
- A partir de ahi se enviaron binds/config reporting a la direccion vieja.
- Esos binds fallaron con `status=0x85`.
- La sesion de reporting termino con timeouts o `missing`, aunque el dispositivo real seguia vivo en `0x2C6D`.
- Despues, reports reales desde `0x2C6D` pudieron verse como procedentes de NWK desconocida, disparando resoluciones IEEE fallidas o ruido innecesario.

## Limitacion real del dispositivo

El `SONOFF SNZB-02D` no parece soportar `POWER_CFG` atributo `0x0020` para voltaje de bateria.

Evidencias:

- `REPORT_CFG_RSP` para `POWER_CFG` `0x0020` devolvio `status=0x86`.
- `READ_ATTR_RSP` para `POWER_CFG` `0x0020` tambien devolvio `status=0x86`.
- `POWER_CFG` `0x0021` si funciono y reporto bateria al 100%.

Conclusion practica: `0x0020` debe tratarse como atributo no soportado para este dispositivo, o saltarse dinamicamente tras verlo fallar.

## Conclusiones validas

1. No hay evidencia de que el sensor se duerma demasiado pronto para configurar reporting durante la union.
2. Si se usa la NWK correcta, hay ventana suficiente para entrevista, bind y configure reporting de los atributos utiles.
3. Poll Control ayudaria si existiera, pero no es imprescindible para este sensor en la union/reunion.
4. El estado `reporting: fallo/parcial` fue correcto como diagnostico visible, pero la causa mezclaba un atributo no soportado real y un bug de direccion NWK.
5. El problema prioritario a corregir es no aceptar announces/updates stale que sobrescriben una NWK recientemente confirmada.
6. La siguiente prueba debe confirmar especificamente que un announce viejo no puede mover el dispositivo de `0x2C6D` a `0x510A` despues de haber recibido respuestas ZDO/ZCL validas.

## Correcciones pendientes recomendadas

- Registrar frescura de NWK confirmada por respuestas reales ZDO/ZCL.
- Ignorar `DEVICE_ANNCE` conflictivos si la NWK actual fue confirmada hace pocos segundos.
- No lanzar configuracion de reporting desde un announce conflictivo descartado.
- Evitar repetir configuracion/probe de atributos ya conocidos como no soportados, especialmente `POWER_CFG` `0x0020`.
- Reducir spam de `IEEE_ADDR_REQ` cuando llegan reports desde una NWK desconocida que no se puede reconciliar inmediatamente.

## Criterio de exito para el siguiente intento

El intento sera bueno si, tras rejoin del sensor:

- la NWK activa queda estable en la direccion nueva;
- las respuestas de `NODE_DESC`, `ACTIVE_EP` y `SIMPLE_DESC` se reciben una sola vez de forma coherente;
- `TEMPERATURE`, `HUMIDITY` y `POWER_CFG` `0x0021` quedan con reporting OK;
- `POWER_CFG` `0x0020` aparece como no soportado o ignorado, no como fallo critico;
- el dispositivo no pasa a offline antes del timeout sleepy configurado si ha reportado recientemente.
