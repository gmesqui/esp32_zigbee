# Entorno de desarrollo en macOS

Preparado el 12 de noviembre de 2026 en un Mac con Apple Silicon.
Repositorio: `/Users/gmesqui/Proyectos/esp32_zigbee`.

## Herramientas

| Herramienta | Versión | Ubicación |
| --- | --- | --- |
| ESP-IDF | 5.5.3 | `~/esp/esp-idf` |
| Compilador RISC-V | GCC 14.2.0, esp-14.2.0_20251107 | `~/.espressif/tools` |
| CMake | 3.30.2 | `~/.espressif/tools/cmake` |
| Ninja | 1.12.1 | `~/.espressif/tools/ninja` |
| Python para ESP-IDF | 3.9.6, entorno virtual independiente | `~/.espressif/python_env/idf5.5_py3.9_env` |
| Node.js | 24.22.3 ARM64 | `~/.local/opt/node` |
| npm | 11.19.1 | Incluido con Node.js |
| Flutter existente | 3.47.9 estable | `~/develop/flutter` |
| Dart | 3.13.7 | Incluido con Flutter |

Xcode 27.2, Git y Visual Studio Code ya estaban instalados. Se han añadido
las extensiones `espressif.esp-idf-extension` y `Dart-Code.flutter`
(que también instala la extensión Dart).

Node.js y el comando `code` se añaden al PATH en `~/.zprofile`.
El alias `get_idf` de `~/.zshrc` activa ESP-IDF cuando hace falta.
En terminales abiertas antes de la instalación, abre una nueva terminal o ejecuta:

```zsh
source ~/.zprofile
source ~/.zshrc
```

## Firmware

```zsh
cd /Users/gmesqui/Proyectos/esp32_zigbee
get_idf
idf.py --version
```

El entorno también puede activarse desde zsh o bash con:

```sh
. "$HOME/esp/esp-idf/export.sh"
```

Las compilaciones de comprobación usan carpetas y configuraciones independientes
para conservar el `sdkconfig` del repositorio:

```sh
# ESP32-C5: conserva los ajustes actuales del proyecto.
mkdir -p build-c5
# Solo la primera vez; no sobrescribir ajustes locales posteriores.
test -f build-c5/sdkconfig || cp sdkconfig build-c5/sdkconfig
idf.py -B build-c5 -D SDKCONFIG="$PWD/build-c5/sdkconfig" build

# ESP32-C6: aplica sdkconfig.defaults y sdkconfig.defaults.esp32c6.
idf.py -B build-c6 -D IDF_TARGET=esp32c6 -D SDKCONFIG="$PWD/build-c6/sdkconfig" build
```

El gestor de componentes actualiza el campo `target` de `dependencies.lock`
al alternar entre placas. Ejecuta las compilaciones de ambas placas de forma secuencial.

Para el flujo habitual del Makefile (`make build`), se usa `build/` y el
`sdkconfig` de la raíz, configurado actualmente para ESP32-C5.

Para listar puertos, con ESP-IDF activado:

```sh
python -m serial.tools.list_ports
```

Para flashear una compilación ya revisada, elige la placa y el puerto adecuados:

```sh
idf.py -B build-c6 -p /dev/cu.usbmodem1101 flash monitor
```

El puerto puede cambiar al reconectar la placa. Sal del monitor con `Ctrl+]`.

## Homebridge e histórico

Las dependencias de ambos paquetes se han instalado a partir de sus lockfiles.

```sh
cd /Users/gmesqui/Proyectos/esp32_zigbee/homebridge-plugin
npm ci
npm test

cd /Users/gmesqui/Proyectos/esp32_zigbee/zigbee-history
npm ci
npm test
npm start
```

El histórico abre en `http://localhost:3090`. El plugin se compila con
`npm run build`; Homebridge está instalado como dependencia local del paquete.

## Simulador Flutter

```sh
cd /Users/gmesqui/Proyectos/esp32_zigbee/tools/homebridge_simulator
flutter pub get
flutter test
flutter build web
flutter run -d web-server --web-hostname 127.0.0.1 --web-port 8088
```

Flutter 3.47.9 requiere las versiones de `matcher`, `meta`, `test_api` y
`vector_math` registradas en el lockfile actualizado. Flutter también ha añadido
exclusiones de carpetas generadas y plataformas al archivo de análisis.
La comprobación del simulador se realiza para web; no requiere Android SDK.

## Comprobaciones y observaciones

- Firmware: compilación completa para ESP32-C5 (`build-c5/`) y ESP32-C6 (`build-c6/`).
- Homebridge: compilación TypeScript y 4 pruebas correctas.
- Histórico: 14 pruebas correctas, incluida la comprobación de peticiones comprimidas
  válidas, malformadas y con bytes sobrantes tras el contenido gzip.
- Simulador: análisis sin incidencias, 1 prueba correcta y compilación web correcta.
- `flutter doctor`: Flutter, Xcode, CocoaPods, Chrome y red disponibles.
  Falta Android SDK para generar aplicaciones Android; el simulador web ya funciona.
- Vulnerabilidades de npm corregidas el 12 de noviembre de 2026. Después de
  reinstalar con `npm ci`, `npm audit` devuelve **0 vulnerabilidades** en ambos
  paquetes, incluidas sus dependencias de desarrollo.

### Actualizaciones de seguridad npm

| Dependencia | Homebridge | Histórico |
| --- | --- | --- |
| `ws` | 8.20.0 → 8.22.2 | 8.21.0 → 8.22.2 |
| `body-parser` | — | 2.3.0 → 2.4.0 |
| `proxy-addr` | — | 2.0.7 → 2.0.8 |
| `qs` | — | 6.15.3 → 6.17.1 |

También se actualiza `content-type` de body-parser a 2.1.0, requerido por
la nueva versión. Los manifiestos de ambos paquetes exigen `ws ^8.22.2`;
los lockfiles fijan las versiones verificadas.

Avisos de los mantenedores:

- [ws: agotamiento de memoria por fragmentación](https://github.com/websockets/ws/security/advisories/GHSA-96hv-2xvq-fx4p).
- [body-parser: caída tras peticiones comprimidas con bytes sobrantes](https://github.com/expressjs/body-parser/security/advisories/GHSA-27r3-jxmh-p828).
- [proxy-addr: suplantación de IP](https://github.com/jshttp/proxy-addr/security/advisories/GHSA-jqcg-44mw-7w3h).
- [qs: error al serializar arrays](https://github.com/ljharb/qs/security/advisories/GHSA-6pq8-jhm2-2vcw).

## Fuentes oficiales

- [Instalación de ESP-IDF 5.5.3 en macOS](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c6/get-started/linux-macos-setup.html)
- [Distribución oficial de Node.js 24.22.3](https://nodejs.org/dist/v24.22.3/)
- [Instalación manual de Flutter](https://docs.flutter.dev/install/manual)
