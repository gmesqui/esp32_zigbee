# Histórico Zigbee ESP32

Aplicación Node independiente para registrar y representar valores publicados por el puente Zigbee ESP32.

## Arranque

```powershell
cd zigbee-history
npm install
npm start
```

Abrir `http://localhost:3090`, indicar la URL del puente (por defecto `ws://esp32-zigbee.local:8080/ws`) y elegir en **Series** qué atributos de qué dispositivos se almacenarán.

Los datos se guardan localmente en `data/zigbee-history.sqlite`. No se envían fuera de la red local. La aplicación necesita Node 22.5 o superior porque usa SQLite integrado en Node.

Al conectarse, el puente publica el inventario y el estado actual. Ese estado se usa para descubrir atributos y mostrar el último valor conocido; posteriormente se registra cada cambio recibido por WebSocket como muestra confirmada.

## Integridad de las gráficas

El estado inicial del puente se guarda como una captura confirmada únicamente si
el propio puente marca el sensor como accesible. Si no lo está, se usa sólo para
descubrir dispositivos y mostrar el último valor conocido. Los eventos Zigbee en
vivo siempre generan muestras confirmadas del histórico.

La base de datos también conserva los intervalos sin captura y la gráfica los
representa con una banda sombreada y una línea cortada. Se registran, entre otros,
los periodos en que la aplicación estuvo detenida, no hubo conexión con el puente,
el sensor no era accesible o la serie se pausó. La ausencia de cambios no crea por
sí sola una banda: sólo se muestran causas de comunicación o captura confirmadas.
