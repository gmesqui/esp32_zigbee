import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:homebridge_simulator/main.dart';

void main() {
  testWidgets('shows ESP32 WebSocket simulator controls', (tester) async {
    await tester.pumpWidget(const HomebridgeSimulatorApp());

    expect(find.text('Homebridge WS Simulator'), findsOneWidget);
    expect(find.text('esp32-zigbee.local'), findsOneWidget);
    expect(find.text('Conectar'), findsOneWidget);
    expect(find.text('Mensajes WS'), findsOneWidget);
    expect(find.text('Enviar JSON'), findsOneWidget);
    expect(find.text('Dispositivos'), findsOneWidget);
    expect(find.byIcon(Icons.send_outlined), findsOneWidget);
  });
}
