# Homebridge WS Simulator

Aplicacion Flutter para probar el transporte WebSocket del ESP32 sin depender
todavia de Homebridge ni de su plugin.

Valores por defecto:

- Host mDNS: `ESP32-zigbee.local`
- URL: `ws://ESP32-zigbee.local:8080/ws`
- Protocolo: mensajes JSON con `type`, `msg_id`, `require_ack` y `data`

Uso rapido:

```powershell
cd tools\homebridge_simulator
flutter run -d windows
```

Tambien puede ejecutarse como web:

```powershell
cd tools\homebridge_simulator
flutter run -d web-server --web-hostname 127.0.0.1 --web-port 8088
```

La pantalla permite conectar/desconectar, cambiar el nombre mDNS o IP,
ver mensajes RX/TX y preparar JSON manualmente o desde plantillas `hello`,
`ping`, `resync` y `cmd` on/off.
