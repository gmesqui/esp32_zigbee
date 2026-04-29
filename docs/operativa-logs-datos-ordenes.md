# Operativa: logs, datos y ordenes

Objetivo: recuperar logs en tiempo real, consultar datos de dispositivos y lanzar ordenes durante pruebas Zigbee.

Gateway usado en las pruebas: `esp32-zigbee.local`.

IP observada en la ultima prueba: `192.168.1.156`.

## Interfaz web y API

La web corre en HTTP puerto `8080`.

- Web principal: `http://esp32-zigbee.local:8080/`
- Dispositivos: `http://esp32-zigbee.local:8080/devices`
- Detalle de un dispositivo: `http://esp32-zigbee.local:8080/device?id=<IEEE>`
- Eventos: `http://esp32-zigbee.local:8080/events`
- Acciones: `http://esp32-zigbee.local:8080/actions`
- Estado JSON completo: `http://esp32-zigbee.local:8080/api/status`
- Configuracion JSON: `http://esp32-zigbee.local:8080/api/config`

Ejemplo para consultar estado:

```powershell
$gw = "esp32-zigbee.local"
$status = Invoke-RestMethod "http://$gw`:8080/api/status"
$status.devices | Select-Object ieee, name, online, state, is_sleepy
```

Ejemplo para ver solo el sensor SNZB-02D:

```powershell
$ieee = "F4B3B1FFFE523696"
$dev = $status.devices | Where-Object { $_.ieee -eq $ieee -or $_.ieee -eq "0x$ieee" }
$dev | ConvertTo-Json -Depth 8
```

## Captura TCP en tiempo real

La consola TCP escucha en el puerto `2323`. Refleja los mismos logs utiles y acepta las mismas teclas que la consola serie.

Conexion interactiva rapida:

```powershell
ncat esp32-zigbee.local 2323
```

Si no resuelve mDNS:

```powershell
ncat 192.168.1.156 2323
```

Captura a fichero con PowerShell:

```powershell
$gw = "esp32-zigbee.local"
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$path = "logs\tcp-console-$stamp.log"
New-Item -ItemType Directory -Force logs | Out-Null
ncat $gw 2323 | Tee-Object -FilePath $path
```

Durante una prueba de rejoin conviene iniciar la captura antes de abrir permit join y dejarla hasta 20-30 segundos despues del ultimo report.

## Comandos de consola TCP/serie

Al conectar, pulsar `?` muestra la ayuda.

Teclas disponibles:

- `1`: lista de dispositivos.
- `2`: estadisticas de red.
- `3`: lista de tareas FreeRTOS.
- `4`: estadisticas de heap.
- `5`: estado de entrevista por logs.
- `g`: leer configuracion de reporting de un atributo, modo interactivo.
- `w`: self-test del protocolo WebSocket.
- `n`: cambiar nombre amigable, modo interactivo.
- `j`: alternar permit join.
- `r`: reentrevistar dispositivo, modo interactivo.
- `e`: borrar cache NVS de dispositivos, pide confirmacion `YES`.
- `?`: ayuda.

## Ordenes HTTP utiles

Abrir permit join:

```powershell
$gw = "esp32-zigbee.local"
Invoke-RestMethod "http://$gw`:8080/api/permit-join" `
  -Method Post `
  -ContentType "application/json" `
  -Body '{"duration_s":180}'
```

Cerrar permit join:

```powershell
Invoke-RestMethod "http://$gw`:8080/api/permit-join" `
  -Method Post `
  -ContentType "application/json" `
  -Body '{"duration_s":0}'
```

Reentrevistar un dispositivo:

```powershell
$deviceId = "F4B3B1FFFE523696"
Invoke-RestMethod "http://$gw`:8080/api/device/reinterview" `
  -Method Post `
  -ContentType "application/json" `
  -Body (@{ device_id = $deviceId } | ConvertTo-Json)
```

Forzar configuracion de reporting:

```powershell
Invoke-RestMethod "http://$gw`:8080/api/device/configure" `
  -Method Post `
  -ContentType "application/json" `
  -Body (@{ device_id = $deviceId } | ConvertTo-Json)
```

Configurar reporting de todos los dispositivos:

```powershell
Invoke-RestMethod "http://$gw`:8080/api/device/configure-all" -Method Post
```

Reiniciar el gateway:

```powershell
Invoke-RestMethod "http://$gw`:8080/api/actions/reboot" -Method Post
```

## Lecturas de reporting por consola

Para leer `Read Reporting Configuration`, usar tecla `g` en TCP/serie.

Valores utiles para el SNZB-02D:

- IEEE: `0xF4B3B1FFFE523696`
- Endpoint: `1`
- Temperatura: cluster `0402`, atributo `0000`
- Humedad: cluster `0405`, atributo `0000`
- Bateria porcentaje: cluster `0001`, atributo `0021`
- Bateria voltaje: cluster `0001`, atributo `0020`

`0001/0020` puede devolver unsupported en este modelo; no debe confundirse con fallo global del reporting.

## Prueba de presencia always-on

Para reproducir falsos offline en routers/always-on, bajar temporalmente los margenes:

```powershell
Invoke-RestMethod "http://$gw`:8080/api/config" -Method Put `
  -ContentType "application/json" `
  -Body (@{
    presence_probe_grace_s = 5
    presence_offline_grace_s = 20
  } | ConvertTo-Json)
```

Buscar en logs:

- `PRESENCE probe ... reason=timeout`
- `RX READ_ATTR_RSP ...` si el dispositivo responde
- `OFFLINE (no contact ...)` solo si no hubo respuesta antes del margen alto

## Checklist para repetir la prueba

1. Arrancar captura TCP a fichero.
2. Consultar `/api/status` y anotar NWK, estado, reporting y `last_seen`.
3. Abrir permit join.
4. Hacer rejoin del sensor.
5. Esperar a que aparezcan `DEVICE_ASSOCIATED`, `DEVICE_ANNCE`, `NODE_DESC`, `ACTIVE_EP`, `SIMPLE_DESC`, binds y `REPORT_CFG_RSP`.
6. Consultar `/api/status` y la pagina del dispositivo.
7. Verificar que las respuestas y reports usan la misma NWK nueva.
8. Revisar si aparecen announces tardios con una NWK vieja y si el firmware los ignora.
