# Actualizar el coordinador Zigbee por Ethernet

El C5 y el C6 reciben la aplicación por el W5500 en el puerto TCP 8082. Homebridge y el panel continúan en el 8080. La herramienta verifica el producto, el chip, la firma y la identidad de la compilación. El dispositivo recibe bloques de 4 KiB en la partición inactiva. Solo cambia el siguiente arranque después de validar la imagen completa y guardar el canal, el PAN ID, el PAN extendido y los IEEE del inventario que tiene que restaurar.

## Una vez por coordinador con la tabla antigua

La migración necesita acceso serie porque el firmware anterior carece de receptor OTA. Identifica la unidad por su chip y su IEEE de coordinador. Antes de apagarla guarda el estado de la API, incluidos canal, PAN ID, PAN extendido y direcciones IEEE de los dispositivos. Usa una carpeta privada nueva por unidad. Los volcados contienen claves de la red.

En los ejemplos se usa el C6. Para el C5 cambia a `chip=esp32c5` y `build=build-c5`. Elige el puerto concreto de ese coordinador. Activa ESP-IDF 5.5.3 desde la raíz del repositorio. Para compilar, usa las [configuraciones C5 y C6](entorno-macos.md#firmware). Comprueba que el bootloader compilado tiene habilitado `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`.

```sh
. "$HOME/esp/esp-idf/export.sh"
chip=esp32c6
build=build-c6
port=/dev/cu.PUERTO_DEL_COORDINADOR
host=esp32-zigbee.local
umask 077
copia=$(mktemp -d "$HOME/zigbee-migracion.XXXXXXXX")
curl -fsS "http://$host:8080/api/status" -o "$copia/antes.json"
python -m esptool --chip "$chip" -p "$port" --after no_reset read_flash 0 0x400000 "$copia/flash.bin"
python tools/check_ota_migration.py "$copia/flash.bin" "$build"
```

Continúa únicamente si terminó el verificador con éxito y reconoces la unidad. Comprueba que en `antes.json` aparecen la red y los dispositivos esperados. El verificador lee la tabla real de la copia, reconoce el producto/chip, comprueba los sectores que va a escribir la compilación y muestra las huellas de las particiones preservadas. Conserva la copia en un lugar privado. Si falla la lectura de los 4 MiB, repítela a 115200 baudios con `--no-stub`, o revisa la conexión antes de escribir.

```sh
idf.py -B "$build" -p "$port" flash
```

Esta primera carga escribe el bootloader, la tabla, los dos sectores de metadatos OTA y la aplicación en `0x40000`. NVS, PHY, `zb_storage` y `zb_fct` conservan sus direcciones y contenido. Evita el borrado completo y el restablecimiento de fábrica. Una interrupción de esta primera escritura puede requerir repetirla por serie. Una vez conectada la placa por Ethernet, compara `/api/status` con la captura anterior y espera actividad de un dispositivo conocido con la admisión de dispositivos cerrada. Conserva esta evidencia antes de continuar con la primera OTA.

## Actualizaciones ordinarias

Desde el entorno Python de ESP-IDF se puede consultar el estado, firmar una compilación nueva y enviarla. La firma de publicación queda junto al binario con extensión `.bin.sig`. Los binarios de C5 y C6 se generan en sus respectivos directorios.

```sh
python tools/coordinator_ota.py status "$host"
python tools/coordinator_ota.py sign "$build/esp32_zigbee3.bin"
python tools/coordinator_ota.py upload "$host" "$build/esp32_zigbee3.bin"
```

Guarda una copia privada de la clave de firma local `~/.config/esp32-zigbee-ota/signing.pem`. Su clave pública está incluida en `main/ota_public.pem`; la herramienta verifica la correspondencia antes de firmar. Para usar la misma clave desde otro equipo, indica su ruta mediante `--key`. El servicio utiliza HTTP en la LAN. La imagen es visible en tránsito. La placa verifica una firma ECDSA P-256 de la imagen y una segunda firma sobre el desafío de un solo uso y el resumen SHA-256. La clave privada permanece en el equipo de administración.

La herramienta espera la partición nueva, el SHA-256 del ELF exacto y la confirmación de arranque. Comprueba el mismo coordinador, canal, PAN y dispositivos previos. El firmware confirma tras recuperar la red Zigbee original y levantar los servicios Ethernet. Si esa prueba falla o caduca a los 120 segundos vuelve a la versión anterior. Si se corta la transferencia antes de seleccionar la imagen, sigue arrancando la versión actual. Tras un resultado incierto, consulta el estado y la partición antes de reintentar.

## Aceptación en banco por cada chip

1. Con la red restaurada y la admisión cerrada, anota el IEEE y el último informe de un sensor o actuador conocido. Migra la unidad y verifica un nuevo informe de esa misma dirección. Un sensor dormido puede tardar hasta su siguiente despertar.
2. Sube una nueva compilación por Ethernet. Verifica el cambio entre `ota_0` y `ota_1`, el SHA del ELF, el inventario y un informe nuevo del equipo ya emparejado. Repite la actualización en sentido inverso entre particiones.
3. Ensaya la imagen del otro chip, una firma incorrecta, un cuerpo truncado y una desconexión durante la transferencia. Comprueba que la partición en ejecución y la red se conservan.
4. En una unidad de banco interrumpe el primer arranque de prueba antes de la confirmación y comprueba que arranca la versión previa y que el dispositivo conocido vuelve a informar. Para probar el plazo de recuperación, desconecta Ethernet en ese primer arranque y reconecta después del plazo. Registra por separado los resultados de C5 y C6.

## Verificación del 14 de noviembre de 2026

ESP-IDF 5.5.3 compiló y firmamos las dos aplicaciones. La imagen C5 ocupa 1 297 392 B y la C6 ocupa 1 209 248 B. Cada partición de aplicación tiene 1 966 080 B. Se verificaron en ambos artefactos el chip, la huella completa del ELF, la firma, el rechazo de una imagen alterada y la configuración de rollback del bootloader. El comprobador de migración se ensayó con volcados sintéticos de 4 MiB. El protocolo de carga, el desafío firmado y la detección de un dispositivo ausente se probaron con transporte simulado.

Para repetir las comprobaciones después de compilar y firmar ambos destinos, activa ESP-IDF y ejecuta `python tools/test_coordinator_ota.py -v`. En la consulta de solo lectura, el coordinador de la LAN ejecutaba la aplicación anterior en `factory`, en el canal 14, con tres dispositivos registrados. La captura previa está en `logs/hardware/ota/`. Quedan pendientes las pruebas en placa descritas arriba, incluido el informe de un dispositivo ya emparejado después de actualizar.
