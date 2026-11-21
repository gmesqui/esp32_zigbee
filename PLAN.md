# Plan de actualización de firmware por red

> **Cierre Sonoff0.5.23:** LAN/WS recuperados tras el reintento automático; RedCasa → RedCasa2/rev16 por elección posterior confirmada del usuario. HAP running/paired=true; panel real comprobado con picos de latencia pendientes. [Reintentos y resultado](../../Documents/ChatGPT/Zigbee/REINTENTOS-WIFI-20261116.md). Las notas de espera inferiores son históricas.


> **Transporte vigente16/11/2026:** C5 `homekit-plain-20261116` confirmado por OTA; HTTP80 y HTTP/WS8765, sin TLS. Sonoff0.5.22 recibido por OTA y arrancado por UART con datos exactos; recuperación LAN/WS pendiente tras201, reconexión sin botón solicitada. HAP y firmas OTA conservados. [Estado real](../../Documents/ChatGPT/Zigbee/SIN-CIFRADO-20261116.md). Los estados TLS inferiores son históricos.


> **Puente HomeKit16/11/2026:** administración en **http://esp32-homekit.local/** (80), login por red guardada, sin excepción de certificado. C5 homekit-http-20261116c confirmado por OTA; Sonoff0.5.21/rev15 sin cambio de firmware. WSS8765/HAP cifrados conservados. [Informe](../../Documents/ChatGPT/Zigbee/ACCESO-WEB-HOMEKIT-20261116.md). Coordinador: esp32-zigbee.local.

> **Cierre de fiabilidad 16/11/2026:** HomeKit C5 bridge-reliable-20261116b confirmado por OTA de navegador; Sonoff 0.5.21/rev12 por red, guardados/reinicio/HAP y mDNS/WSS automático comprobados con datos originales. Mac intacto. [Informe conjunto](../../Documents/ChatGPT/Zigbee/FIABILIDAD-CORREGIDA-20261116.md). No es una nueva carga ni aceptación completa del coordinador.

> **Recapitulación, 16/11/2026:** [estado, pruebas y fiabilidad Sonoff/HomeKit](../../Documents/ChatGPT/Zigbee/ESTADO-Y-FIABILIDAD-20261116.md). Último cierre: Sonoff 0.5.16/revisión 8, web/WSS, dos OTA Wi‑Fi; C5 sonoff-admin-20261115, ota_1 confirmado. La OTA C5 desde navegador falla por memoria. Las fases históricas inferiores no son pausas ni versiones actuales. Esta revisión no añade una nueva aceptación del coordinador ni de su OTA.

## Norma general del proyecto

Todo dispositivo con microcontrolador, actual o futuro, debe poder instalar una nueva versión de su propio firmware por su red de comunicaciones, sin conectarlo físicamente a un ordenador. La conexión física queda como último recurso para recuperar un equipo o para habilitar por primera vez la actualización por red cuando el equipo carezca de esa capacidad. En cada diseño se elegirá el transporte que corresponda a su red y se reservará espacio para actualizar desde el principio.

El criterio de aceptación es actualizar una unidad de la versión anterior a la nueva por red, reiniciarla y consultar por red la versión en ejecución. Se comprobará que conserva su configuración y sigue prestando su función. Para un modelo comercial con firmware del fabricante, documentar y comprobar su vía de actualización antes de incorporarlo.

## Estado Sonoff — OTA comprobada, 15 de noviembre de 2026

Sonoff **0.5.16 con TLS reducido instalado por OTA web**. Tras reanudar el usuario las pruebas se acreditaron **0.5.14 → 0.5.15 → 0.5.16 por Wi-Fi**, con arranques nuevos, perfiles/orden/E/S/identidad conservados y retorno mDNS/WSS automático. El puente continúa en **sonoff-admin-20261115**; tras la reinstalación posterior por cliente HTTPS está en **ota_1 confirmado** (cierre del 16/11). La consola USB de aplicación permite carga firmada, diagnóstico, reset y ROM sin botón mientras el firmware responda; no sustituye la actualización ordinaria por red.

El puente HomeKit usa DHCP y `homekit-c5-<MAC>.local`; los Sonoff lo descubren por mDNS, sin depender de una IP fija. El coordinador mantiene `esp32-zigbee.local`. [Inventario y aceptación futura](../esp8266_sonoff/IMPLEMENTACION-20261115.md), [plan Sonoff](../esp8266_sonoff/PLAN.md) y [plan HomeKit](../esp32_homekit_wifi/PLAN.md). No borrar perfiles ni cambiar la Wi-Fi del Mac; RTOS continúa aparcado.

La entrega 0.5.16 mide **491840 B de aplicación, 349604 B firmados, recepción OTA 401408 B y margen 51804 B**. LittleFS 128 KiB conservado. El actualizador web usa bloques confirmados y comprueba el arranque, con la misma firma y autenticación. Pasan 29 pruebas Python/C++ y siete del actualizador; además se verificaron en placa ambas OTA y vuelta del vínculo. [Causas, evidencia y límites pendientes](../esp8266_sonoff/OTA-CORREGIDA-20261115.md). No se acreditan todavía 48 h ni cortes durante eboot/mediciones eléctricas. Mac sin cambios de Wi-Fi.

## Configuración Wi-Fi común, 14–15 de noviembre de 2026 — resultados anteriores

Sonoff **0.4.10** y HomeKit **wifi-20261114** incluyen lista de redes con selección y Subir/Bajar, búsqueda dinámica, alta y eliminación. Sin perfiles, web libre; con alguno, SSID y contraseña de cualquiera de los guardados, aunque no sea la conexión activa. AP abierto y autenticación web. El coordinador solo usa Ethernet y no necesita ese cambio. [Requisitos y resultados](../../Documents/ChatGPT/Zigbee/CAMBIOS-WIFI.md).

HomeKit está confirmado en ota_1 tras HTTPS, con identidad, HAP, dispositivos y reglas conservados. Imagen 1531904 B, particiones 1966080 B, margen 434176 B. Sonoff recibió por LAN 0.4.2 → 0.4.3 → 0.4.5 y finalmente **0.4.9 → 0.4.10**, conservando perfiles/orden, ocho parámetros y clave. La recuperación intermedia 0.4.9 por USB fue expresamente solicitada y conservó LittleFS/SDK byte a byte. La entrega 0.4.10 tiene aplicación 520624 B, firmado 370824 B, recepción medida 372736 B y margen 1912 B. Incorpora relé/pulsador en el panel y un reintento acotado que recupera RedCasa2 del rechazo inicial 202. Se verificaron dos reinicios de software y Power On con USB y alimentación normal; el primer rechazo puede seguir como histórico. Pasan 19 pruebas Python/C++ y siete de navegador, además de ensayos sobre placa. El Mac mantuvo su Wi-Fi. Quedan estabilidad prolongada, WSS activo y carga eléctrica. [Registro](../esp8266_sonoff/CONTINUAR.md).

Los apartados siguientes conservan el diseño y las mediciones anteriores; el estado vigente es el indicado arriba y en los registros de cada equipo.

## Trabajo sobre los firmwares existentes

La revisión de fuentes, tablas de particiones y binarios locales del 14 de noviembre de 2026 identificó tres firmwares que requerían adaptación. La pasarela Zigbee C5/C6 y el controlador HomeKit C5 ya disponen de implementación y compilaciones OTA. El estado y el procedimiento de HomeKit están en su [guía OTA](../esp32_homekit_wifi/OTA.md). La migración y los ensayos Wi-Fi de HomeKit están completados según su registro; la aceptación con red Zigbee y dispositivos emparejados permanece pendiente para el coordinador. Véase su [guía](docs/ota-ethernet.md).

| Firmware | Red para actualizar | Imagen de aplicación revisada | Trabajo específico |
|---|---|---:|---|
| Coordinador Zigbee ESP32-C5 / ESP32-C6 | Ethernet W5500 | C5 1 212 448 B · C6 1 123 392 B | Carga autenticada por Ethernet implementada para ambos chips. Los tamaños de esta tabla corresponden a la revisión anterior a OTA. El WebSocket actual sirve a Homebridge y al controlador. La consulta de la partición en ejecución en `main/ws_transport.c` solo ofrece diagnóstico. |
| Controlador autónomo HomeKit ESP32-C5 | Ethernet W5500 o Wi-Fi doméstica | 1 563 088 B · margen 402 992 B por slot | sonoff-admin-20261115 confirmado en ota_1 por cliente HTTPS, con red/identidad/HAP/AID/IID/reglas conservados. Negativos/interrupción probados. Navegador falla por memoria; Ethernet/Casa/estabilidad pendientes. |
| Sonoff BASIC ESP8266, 0.5.16 instalada | Wi-Fi doméstica y AP de recuperación | 491 840 B aplicación · 349 604 B firmado · recepción 401 408 B · margen 51 804 B | Dos OTA consecutivas por Wi-Fi, segunda desde navegador, con datos y vuelta mDNS/WSS confirmados. Consola USB de recuperación probada. Pendientes matrices de fallos, estabilidad y electricidad. |

### 1. Diseño inicial de migración ESP32 (histórico)

Las tablas al comenzar contenían una sola aplicación `factory` en una flash de 4 MiB. Como propuesta de partida, el hueco anterior a la aplicación admite `otadata` (8 KiB en 0x3C000) y el espacio desde 0x40000 admite `ota_0` y `ota_1` de 1 966 080 B cada una. Las tres imágenes medidas caben. Verificar los tamaños tras añadir el actualizador y la firma. Conservar los offsets de NVS y PHY y, en el coordinador, los de `zb_storage` y `zb_fct`.

Preparar y probar en banco la carga inicial de bootloader, tabla y aplicación con copia previa de los datos persistentes y sin borrado general. Después, escribir por red en la partición inactiva, verificar firma, producto y destino, y confirmar la nueva versión tras una comprobación local de arranque. Habilitar la vuelta a la versión anterior si falla esa comprobación. En el coordinador, comprobar que mantiene su red y que un dispositivo ya unido vuelve a informar. En HomeKit, comprobar emparejamientos, identidades de accesorios, enlaces Sonoff y reglas sin repetir acciones al reiniciar. [OTA de ESP-IDF 5.5.3](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c5/api-reference/system/ota.html)

### 2. Resolver primero la capacidad del Sonoff

En el perfil provisional de 1 MiB con 128 KiB para LittleFS (`eagle.flash.1m128.ld`), quedarían 380 928 B para recibir una actualización con la aplicación 0.2.0 instalada, una vez alineada a sectores. Su binario sin comprimir supera ese espacio. Comprimido con `gzip -9n`, ocupa 372 743 B antes de la firma. El margen es de solo 8 185 B y la versión medida aún carece de actualizador.

Construir primero una versión con el actualizador, medir en placa el espacio que anuncia `ESP.getFreeSketchSpace()` y probar **dos actualizaciones sucesivas** con paquetes comprimidos y firmados. Así se verifica también que la primera versión actualizable deja espacio para su sucesora. Si no caben, reducir la aplicación o validar una imagen intermedia mínima que permita completar ambos saltos por Wi-Fi. Actualizar solo la aplicación, conservar el vínculo y los perfiles de E/S, y posponer el reinicio hasta que termine cualquier pulso del relé. Ensayar por separado los cortes durante la descarga y durante la copia de arranque de `eboot`, y documentar cuándo hace falta recuperación por serie. [OTA del núcleo Arduino ESP8266](https://arduino-esp8266.readthedocs.io/en/3.1.2/ota_updates/readme.html)

### 3. Entrega y pruebas de aceptación

Publicar por versión y producto la imagen firmada y su identificación de hardware. La herramienta o panel debe mostrar versión instalada, progreso y resultado tras reconectar. En banco, probar desde la versión anterior una imagen válida, otra truncada y otra de producto incorrecto, además de una interrupción de red y otra de alimentación. Registrar la versión que arranca, los datos conservados y cómo se recupera cada modelo. Las unidades antiguas que todavía carezcan de receptor OTA necesitan una primera habilitación local. Las unidades del banco ya disponen de actualización por red; las actualizaciones ordinarias posteriores se harán por ella. Los ensayos pendientes se rigen por los planes actuales Sonoff/HomeKit y la petición vigente del usuario; las pausas de fases anteriores son históricas.
