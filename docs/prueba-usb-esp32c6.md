# Prueba del firmware por USB — 12 de noviembre de 2026

## Dispositivo y carga

- Chip identificado por esptool: **ESP32-C6FH4**, revisión **v0.2**, flash integrada de **4 MB**.
- Puerto: `/dev/cu.usbmodem1101`, USB Serial/JTAG, VID/PID `303A:1001`.
- Configuración de placa del proyecto: `ESP32-C6 Super Mini`.
- Firmware: `build-c6/esp32_zigbee3.bin`, compilado con ESP-IDF 5.5.3.
- Bootloader, tabla de particiones y aplicación escritos y verificados por esptool.
- SHA-256 de la aplicación: `d3dda7da70d53ad0a6bd3949cbfb19736907ed3f9c91aa7761b9e35db812e54f`.

## Correcciones encontradas en la placa

1. La orden `w` provocaba `Stack protection fault` en `serial_cmd_task`, durante
   el formateo de datos de la autoprueba. Se aumenta su pila de 4096 a 8192 bytes
   para alojar las llamadas anidadas, los buffers de mensajes y el formateo numérico.
2. Las ejecuciones posteriores de la prueba de sesión daban un falso fallo al
   esperar identificadores 1 y 2. El contador es global y continúa entre sesiones;
   la prueba comprueba ahora identificadores consecutivos no nulos y su retorno
   a 1 cuando se agota el contador.

Ambas correcciones se han compilado para ESP32-C5 y ESP32-C6. La versión corregida
para C6 está cargada en el dispositivo.

## Resultados

- Arranque completo e inicialización del coordinador Zigbee.
- Red existente restaurada en canal 14 y dos dispositivos recuperados de NVS.
- Ayuda, listado de dispositivos, estadísticas de red y memoria por consola operativos.
- Autoprueba WebSocket repetida **4 veces consecutivas**: cada ejecución termina
  con `passed=9 failed=0 skipped=0`.
- Memoria libre estable en **177544 bytes** antes y después de las repeticiones;
  mínimo observado durante las pruebas: **170420 bytes**.
- Los dos sensores guardados aparecen offline; no se han recibido informes de
  sensores durante esta comprobación.
- El controlador W5500 se inicializa, pero no se observa enlace Ethernet ni IP
  asignada. La conexión WebSocket desde el Mac queda pendiente de disponer de red.

## Registros locales

Los registros se guardan en `logs/hardware/` y están excluidos de Git:

- `build-and-flash-final.log`: compilaciones C5/C6 y carga final.
- `c6-selftest-stack-fault.txt`: diagnóstico del fallo original.
- `c6-first-stack-fix.txt`: comprobación del aumento de pila.
- `c6-usb-validation.txt`: arranque y pruebas de la versión final.
