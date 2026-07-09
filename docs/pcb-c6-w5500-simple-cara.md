# PCB simple cara ESP32-C6 Super Mini + W5500 Lite

Objetivo: placa adaptadora de una sola cara para montar el `ESP32-C6 Super Mini`
y el `W5500 Lite` lado a lado, con el USB-C y el RJ45 alineados en el mismo
borde, minimizando puentes sin cambiar el firmware actual.

## Supuestos mecanicos

- Vista superior: USB-C del C6 y RJ45 del W5500 mirando hacia arriba.
- W5500 usado: modulo `W5500 Lite` / `HW-884`, 12 pines, dos tiras de 1x6.
- Montaje recomendado: C6 a la derecha, W5500 a la izquierda, con las tiras de
  pines enfrentadas hacia el centro de la PCB.
- Cara de cobre: inferior.
- Headers: hembra para poder sustituir placas, o taladros directos si quieres
  soldarlas permanente.
- Mantener libre la zona de antena del C6: no poner cobre, plano de masa ni
  W5500 bajo la antena. Si el USB queda arriba, deja la antena sobresaliendo
  hacia el borde exterior del C6.

## Pinout electrico

Este diseno sigue `main/board_config.h`:

| Senal | ESP32-C6 Super Mini | W5500 Lite |
|-------|----------------------|------------|
| 3V3 | `3V3` | `V` pin 8 y `V` pin 9 |
| GND | `GND` | `G` pins 1, 2 y 7 |
| MOSI | `GPIO2` | `MO` pin 3 |
| MISO | `GPIO3` | `MI` pin 12 |
| SCLK | `GPIO6` | `SCK` pin 4 |
| CS | `GPIO7` | `CS` pin 5 |
| INT | `GPIO18` | `INT` pin 6 |
| RST | `GPIO14` | `RST` pin 11 |

No conectes `5V` al W5500 Lite. Esta variante no lleva regulador de entrada
para alimentar el modulo desde 5 V; los pines `V` son de 3.3 V.

## Alimentacion

La alimentacion queda cableada asi:

- `GND` del C6 a todos los `G/GND` del W5500.
- `3V3` del C6 a los dos pines `V` del W5500.
- Pista de `3V3`: 0.8 mm minimo; 1.0 mm si cabe.
- Pista de `GND`: 1.0 mm minimo o relleno de cobre en la cara inferior.

El W5500 Lite puede rondar 150-200 mA. Alimentalo desde el pin `3V3` del C6 y
mantén la pista de alimentacion lo mas corta y ancha posible.

## Colocacion recomendada

```
Vista superior, conectores arriba

        borde de conectores
  +-------------------------------+
  |  RJ45             USB-C       |
  | +--------+      +--------+    |
  | | W5500  |      |  C6    |    |
  | | Lite   |      | Super  |    |
  | |        |      | Mini   |    |
  | +--------+      +--------+    |
  |                               |
  +-------------------------------+
       cara inferior = cobre
```

Separacion entre placas: 2.54 mm a 5.08 mm. Cuanto menor sea, mas cortas quedan
las pistas SPI, pero deja al menos espacio para soldar los puentes.

## Ruteo simple cara

Prioridad de ruteo:

1. `GND`: relleno inferior, conectando al menos dos GND del W5500.
2. `3V3`: pista ancha por el borde superior/interior hacia los dos pines `V`.
3. SPI rapido: `SCLK`, `MOSI`, `MISO` lo mas corto posible y sin rodeos largos.
4. Control: `CS`, `INT`, `RST`.

Rutas en cobre inferior:

| Net | Ruta propuesta |
|-----|----------------|
| `MOSI` | `GPIO2 -> MO`, directa por el pasillo interior |
| `SCLK` | `GPIO6 -> SCK`, directa y corta |
| `CS` | `GPIO7 -> CS`, directa y corta |
| `RST` | `GPIO14 -> RST`, por el borde inferior si no cruza SPI |
| `3V3` | `C6 3V3 -> rail -> V8/V9`, pista ancha |
| `GND` | plano/relleno inferior, con thermal relief en headers |

Puentes previstos:

| Puente | Net | Motivo | Implementacion |
|--------|-----|--------|----------------|
| `JP1` | `MISO GPIO3 -> MI` | Cruza la zona de `SCLK/CS` en simple cara | hilo aislado corto por arriba |
| `JP2` | `INT GPIO18 -> INT` | `GPIO18` queda en la otra hilera del C6 | hilo aislado corto por arriba |

Con esta colocacion deberian bastar 2 puentes. Si al dibujarlo exacto en KiCad
la pista `RST` queda bloqueada, conviertela en `JP3`; es una senal lenta y no
importa que vaya por puente.

## Intercambiar los lados

Manteniendo USB-C y RJ45 en el mismo borde, intercambiar los lados significa:

```
Actual:     W5500 a la izquierda, C6 a la derecha
Invertida:  C6 a la izquierda, W5500 a la derecha
```

Con el pinout actual del firmware, la orientacion actual sigue siendo la mejor
para simple cara:

| Colocacion | Pistas directas mas favorables | Puentes esperados |
|------------|--------------------------------|-------------------|
| W5500 izquierda, C6 derecha | `MOSI`, `SCLK`, `CS`, `RST`, `3V3`, `GND` | 2: `MISO`, `INT` |
| C6 izquierda, W5500 derecha | `INT`, parte de `3V3/GND`, a veces `RST` | 3-4: normalmente `MOSI`, `MISO`, `SCLK`, `CS` |

La razon es que, al invertirlos, el lado del W5500 con `MO/SCK/CS/INT` queda
enfrentado al lado del C6 donde estan `GPIO18/GPIO14/3V3`, pero las senales SPI
principales del C6 (`GPIO2`, `GPIO3`, `GPIO6`, `GPIO7`) quedan en el lado
opuesto. Ganas `INT`, pero pierdes varias pistas SPI, y eso no compensa.

Por tanto, si no cambiamos `board_config.h`, no merece la pena intercambiar los
lados.

## Huellas y reglas

- Taladros headers: 1.0 mm para pines cuadrados de 0.64 mm.
- Paso headers: 2.54 mm.
- Pistas SPI: 0.25-0.30 mm minimo; mejor 0.4 mm si hay sitio.
- Separacion: 0.25 mm minimo para fabricacion normal; 0.4 mm si vas a fresar o
  atacar en casa.
- Sin vias: al ser simple cara, cualquier cruce se resuelve como puente de hilo.
- Etiquetas serigrafia imprescindibles: `3V3`, `GND`, `MO`, `MI`, `SCK`, `CS`,
  `INT`, `RST`, `GPIO2`, `GPIO3`, `GPIO6`, `GPIO7`, `GPIO18`, `GPIO14`.

## Archivo vectorial

El cobre inferior esta en:

- `docs/pcb-c6-w5500-cobre-inferior.svg`
- `hardware/c6-w5500-simple-cara/c6-w5500-simple-cara.kicad_pcb`

El SVG esta a escala 1:1, con tamano de placa `85 mm x 52 mm`. Las formas
negras son cobre; los circulos blancos son los taladros. Las lineas rojas
discontinuas (`JP1` y `JP2`) son los dos puentes de hilo y no deben transferirse
como cobre.

El archivo KiCad contiene la misma placa con borde `Edge.Cuts`, pads pasantes,
redes nombradas y pistas en `B.Cu`. Los puentes `JP1` y `JP2` aparecen solo como
serigrafia/guia.

Antes de atacar o fresar, imprime en papel normal al 100% y comprueba con las
placas reales que los headers de 2.54 mm encajan y que los conectores USB-C y
RJ45 quedan alineados como esperas.

## Variante con menos puentes tocando firmware

Si aceptas cambiar pines en `main/board_config.h`, puedes llevar `INT` y `RST`
a pines del lado del C6 que quede junto al W5500. Por ejemplo:

| Senal | Pin actual | Pin alternativo |
|-------|------------|-----------------|
| `INT` | `GPIO18` | `GPIO4` |
| `RST` | `GPIO14` | `GPIO5` |

Esto puede dejar el diseno en 1 puente o incluso 0, segun la orientacion exacta
del modulo. La contra: `GPIO4` y `GPIO5` son pines de strapping/JTAG en ESP32-C6,
asi que solo los usaria si el W5500 no los fuerza durante reset. Para una placa
estable prefiero mantener el pinout actual y aceptar los dos puentes.

## Checklist antes de fabricar

- Confirmar con multimetro que el W5500 de tu lote coincide con el pinout:
  `G, G, MO, SCK, CS, INT` en un lado y `G, V, V, NC, RST, MI` en el otro.
- Confirmar continuidad `3V3 -> V/V`, `GND -> G/GND`.
- Confirmar que no hay continuidad entre `5V` y `3V3`.
- Confirmar que `RST` tiene pull-up o queda alto al arrancar; el driver lo
  controlara por `GPIO14`.
- Probar primero alimentado por USB sin cable Ethernet, luego con Ethernet.
- En monitor serie, buscar:
  `ETH: W5500 driver started ... MOSI=2 MISO=3 SCLK=6 CS=7 INT=18 RST=14`.

## Referencias

- Firmware local: `main/board_config.h`.
- Pinout C6 Super Mini: https://www.espboards.dev/esp32/esp32-c6-super-mini/
- Datasheet ESP32-C6-MINI-1: https://www.espressif.com/sites/default/files/documentation/esp32-c6-mini-1_datasheet_en.pdf
- Pinout W5500 Lite / HW-884: https://done.land/components/data/datatransmission/wired/ethernet/miniw5500/
