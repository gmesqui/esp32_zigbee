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

Al conectarse, el puente publica el inventario y el estado actual. Ese estado se usa para descubrir atributos y como primera muestra de una serie ya configurada; posteriormente se registra cada cambio recibido por WebSocket.
