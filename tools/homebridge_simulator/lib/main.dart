import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:web_socket_channel/status.dart' as ws_status;
import 'package:web_socket_channel/web_socket_channel.dart';

void main() {
  runApp(const HomebridgeSimulatorApp());
}

const _defaultMdnsName = 'ESP32-zigbee.local';
const _defaultPort = '8080';
const _defaultPath = '/ws';
const _protocolVersion = 1;

class HomebridgeSimulatorApp extends StatelessWidget {
  const HomebridgeSimulatorApp({super.key});

  @override
  Widget build(BuildContext context) {
    final colorScheme = ColorScheme.fromSeed(
      seedColor: const Color(0xFF176B68),
      brightness: Brightness.light,
    );

    return MaterialApp(
      debugShowCheckedModeBanner: false,
      title: 'Homebridge WS Simulator',
      theme: ThemeData(
        colorScheme: colorScheme,
        scaffoldBackgroundColor: const Color(0xFFF6F8F7),
        useMaterial3: true,
        inputDecorationTheme: const InputDecorationTheme(
          border: OutlineInputBorder(),
          isDense: true,
        ),
      ),
      home: const SimulatorScreen(),
    );
  }
}

class SimulatorScreen extends StatefulWidget {
  const SimulatorScreen({super.key});

  @override
  State<SimulatorScreen> createState() => _SimulatorScreenState();
}

class _SimulatorScreenState extends State<SimulatorScreen> {
  final _hostController = TextEditingController(text: _defaultMdnsName);
  final _portController = TextEditingController(text: _defaultPort);
  final _pathController = TextEditingController(text: _defaultPath);
  final _jsonController = TextEditingController();
  final _deviceIdController = TextEditingController();
  final _logScrollController = ScrollController();

  WebSocketChannel? _channel;
  StreamSubscription<dynamic>? _subscription;
  final List<_LogEntry> _log = [];
  var _nextMsgId = 1;
  var _useTls = false;
  var _sendHelloOnConnect = false;
  var _connecting = false;
  var _connected = false;

  bool get _canSend => _connected && _channel != null;

  @override
  void initState() {
    super.initState();
    _jsonController.text = _pretty(_helloMessage());
  }

  @override
  void dispose() {
    _subscription?.cancel();
    _channel?.sink.close(ws_status.goingAway);
    _hostController.dispose();
    _portController.dispose();
    _pathController.dispose();
    _jsonController.dispose();
    _deviceIdController.dispose();
    _logScrollController.dispose();
    super.dispose();
  }

  Uri _buildUri() {
    final hostText = _hostController.text.trim();
    if (hostText.startsWith('ws://') || hostText.startsWith('wss://')) {
      return Uri.parse(hostText);
    }

    final port = int.tryParse(_portController.text.trim());
    var path = _pathController.text.trim();
    if (path.isEmpty) {
      path = _defaultPath;
    }
    if (!path.startsWith('/')) {
      path = '/$path';
    }

    return Uri(
      scheme: _useTls ? 'wss' : 'ws',
      host: hostText.isEmpty ? _defaultMdnsName : hostText,
      port: port,
      path: path,
    );
  }

  Future<void> _connect() async {
    if (_connecting || _connected) {
      return;
    }

    final uri = _buildUri();
    setState(() => _connecting = true);
    _addLog(_LogDirection.system, 'Conectando a $uri');

    try {
      final channel = WebSocketChannel.connect(uri);
      await channel.ready.timeout(const Duration(seconds: 5));
      _subscription = channel.stream.listen(
        _handleIncoming,
        onDone: _handleDisconnected,
        onError: (Object error) {
          _addLog(_LogDirection.error, 'Error WS: $error');
          _handleDisconnected();
        },
      );

      setState(() {
        _channel = channel;
        _connected = true;
        _connecting = false;
      });
      _addLog(_LogDirection.system, 'Conectado');

      if (_sendHelloOnConnect) {
        _sendObject(_helloMessage());
      }
    } on Object catch (error) {
      setState(() => _connecting = false);
      _addLog(_LogDirection.error, 'No se pudo conectar: $error');
    }
  }

  Future<void> _disconnect() async {
    final channel = _channel;
    _subscription?.cancel();
    _subscription = null;
    _channel = null;

    if (channel != null) {
      await channel.sink.close(ws_status.goingAway);
    }

    if (mounted) {
      setState(() => _connected = false);
    }
    _addLog(_LogDirection.system, 'Desconectado');
  }

  void _handleDisconnected() {
    if (!_connected && !_connecting) {
      return;
    }

    setState(() {
      _connected = false;
      _connecting = false;
      _channel = null;
    });
    _addLog(_LogDirection.system, 'Conexion cerrada por el ESP32');
  }

  void _handleIncoming(dynamic message) {
    final text = message?.toString() ?? '';
    _addLog(_LogDirection.rx, text);
  }

  void _sendJson() {
    if (!_canSend) {
      _addLog(_LogDirection.error, 'No hay conexion activa');
      return;
    }

    final text = _jsonController.text.trim();
    if (text.isEmpty) {
      _addLog(_LogDirection.error, 'El JSON esta vacio');
      return;
    }

    try {
      jsonDecode(text);
    } on FormatException catch (error) {
      _addLog(_LogDirection.error, 'JSON invalido: ${error.message}');
      return;
    }

    _channel!.sink.add(text);
    _addLog(_LogDirection.tx, text);
  }

  void _sendObject(Map<String, dynamic> message) {
    final text = _pretty(message);
    _channel?.sink.add(text);
    _addLog(_LogDirection.tx, text);
  }

  void _applyTemplate(_MessageTemplate template) {
    final deviceId = _deviceIdController.text.trim();
    final message = switch (template) {
      _MessageTemplate.hello => _helloMessage(),
      _MessageTemplate.ping => {
        'type': 'ping',
        'msg_id': _allocateMsgId(),
        'require_ack': false,
        'data': <String, dynamic>{},
      },
      _MessageTemplate.resync => {
        'type': 'resync',
        'msg_id': _allocateMsgId(),
        'require_ack': true,
        'data': <String, dynamic>{},
      },
      _MessageTemplate.turnOn => _commandMessage(deviceId, true),
      _MessageTemplate.turnOff => _commandMessage(deviceId, false),
    };
    setState(() => _jsonController.text = _pretty(message));
  }

  void _formatComposerJson() {
    try {
      final value = jsonDecode(_jsonController.text);
      setState(() => _jsonController.text = _pretty(value));
    } on FormatException catch (error) {
      _addLog(_LogDirection.error, 'JSON invalido: ${error.message}');
    }
  }

  void _clearLog() {
    setState(_log.clear);
  }

  Map<String, dynamic> _helloMessage() {
    return {
      'type': 'hello',
      'msg_id': _allocateMsgId(),
      'require_ack': false,
      'data': {
        'protocol_version': _protocolVersion,
        'client': 'flutter-homebridge-simulator',
      },
    };
  }

  Map<String, dynamic> _commandMessage(String deviceId, bool state) {
    return {
      'type': 'cmd',
      'msg_id': _allocateMsgId(),
      'require_ack': true,
      'data': {
        'device_id': deviceId.isEmpty ? '00124B0000000000' : deviceId,
        'cluster': 'onoff',
        'command': 'set',
        'params': {'state': state},
      },
    };
  }

  int _allocateMsgId() => _nextMsgId++;

  void _addLog(_LogDirection direction, String rawText) {
    final entry = _LogEntry(
      direction: direction,
      timestamp: DateTime.now(),
      text: _formatPayload(rawText),
      messageType: _extractMessageType(rawText),
    );

    setState(() {
      _log.add(entry);
      if (_log.length > 500) {
        _log.removeRange(0, _log.length - 500);
      }
    });

    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!_logScrollController.hasClients) {
        return;
      }
      _logScrollController.animateTo(
        _logScrollController.position.maxScrollExtent,
        duration: const Duration(milliseconds: 180),
        curve: Curves.easeOut,
      );
    });
  }

  String _formatPayload(String rawText) {
    try {
      return _pretty(jsonDecode(rawText));
    } on Object {
      return rawText;
    }
  }

  String? _extractMessageType(String rawText) {
    try {
      final value = jsonDecode(rawText);
      if (value case {'type': final String type}) {
        return type;
      }
    } on Object {
      return null;
    }
    return null;
  }

  String _pretty(Object? value) {
    return const JsonEncoder.withIndent('  ').convert(value);
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Homebridge WS Simulator'),
        actions: [
          Padding(
            padding: const EdgeInsets.only(right: 12),
            child: _StatusPill(connected: _connected, connecting: _connecting),
          ),
        ],
      ),
      body: SafeArea(
        child: LayoutBuilder(
          builder: (context, constraints) {
            final wide = constraints.maxWidth >= 920;
            final content = wide
                ? Row(
                    crossAxisAlignment: CrossAxisAlignment.stretch,
                    children: [
                      Expanded(flex: 6, child: _buildLogPanel()),
                      const SizedBox(width: 16),
                      SizedBox(width: 430, child: _buildComposerPanel()),
                    ],
                  )
                : SingleChildScrollView(
                    child: Column(
                      children: [
                        SizedBox(height: 360, child: _buildLogPanel()),
                        const SizedBox(height: 16),
                        SizedBox(height: 560, child: _buildComposerPanel()),
                      ],
                    ),
                  );

            return Padding(
              padding: const EdgeInsets.all(16),
              child: Column(
                children: [
                  _buildConnectionPanel(),
                  const SizedBox(height: 16),
                  Expanded(child: content),
                ],
              ),
            );
          },
        ),
      ),
    );
  }

  Widget _buildConnectionPanel() {
    return _Panel(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              const Icon(Icons.router_outlined),
              const SizedBox(width: 8),
              Text(
                'Conexion ESP32',
                style: Theme.of(context).textTheme.titleMedium,
              ),
              const Spacer(),
              Tooltip(
                message: 'Limpiar mensajes',
                child: IconButton(
                  onPressed: _log.isEmpty ? null : _clearLog,
                  icon: const Icon(Icons.delete_sweep_outlined),
                ),
              ),
            ],
          ),
          const SizedBox(height: 12),
          Wrap(
            spacing: 12,
            runSpacing: 12,
            crossAxisAlignment: WrapCrossAlignment.center,
            children: [
              SizedBox(
                width: 260,
                child: TextField(
                  controller: _hostController,
                  enabled: !_connected && !_connecting,
                  decoration: const InputDecoration(
                    labelText: 'Nombre mDNS o IP',
                    prefixIcon: Icon(Icons.dns_outlined),
                  ),
                ),
              ),
              SizedBox(
                width: 110,
                child: TextField(
                  controller: _portController,
                  enabled: !_connected && !_connecting,
                  keyboardType: TextInputType.number,
                  decoration: const InputDecoration(labelText: 'Puerto'),
                ),
              ),
              SizedBox(
                width: 120,
                child: TextField(
                  controller: _pathController,
                  enabled: !_connected && !_connecting,
                  decoration: const InputDecoration(labelText: 'Ruta'),
                ),
              ),
              SegmentedButton<bool>(
                segments: const [
                  ButtonSegment(
                    value: false,
                    icon: Icon(Icons.lock_open_outlined),
                    label: Text('ws'),
                  ),
                  ButtonSegment(
                    value: true,
                    icon: Icon(Icons.lock_outline),
                    label: Text('wss'),
                  ),
                ],
                selected: {_useTls},
                onSelectionChanged: _connected || _connecting
                    ? null
                    : (value) => setState(() => _useTls = value.first),
              ),
              FilterChip(
                selected: _sendHelloOnConnect,
                onSelected: _connected
                    ? null
                    : (value) => setState(() => _sendHelloOnConnect = value),
                label: const Text('hello al conectar'),
                avatar: const Icon(Icons.waving_hand_outlined, size: 18),
              ),
              FilledButton.icon(
                onPressed: _connected
                    ? _disconnect
                    : (_connecting ? null : _connect),
                icon: Icon(
                  _connected ? Icons.link_off_outlined : Icons.link_outlined,
                ),
                label: Text(_connected ? 'Desconectar' : 'Conectar'),
              ),
            ],
          ),
          const SizedBox(height: 10),
          Text(
            _buildUri().toString(),
            style: Theme.of(context).textTheme.bodySmall?.copyWith(
              color: Theme.of(context).colorScheme.onSurfaceVariant,
              fontFamily: 'monospace',
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildLogPanel() {
    return _Panel(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              const Icon(Icons.article_outlined),
              const SizedBox(width: 8),
              Text(
                'Mensajes WS',
                style: Theme.of(context).textTheme.titleMedium,
              ),
              const Spacer(),
              Text(
                '${_log.length}/500',
                style: Theme.of(context).textTheme.labelMedium,
              ),
            ],
          ),
          const SizedBox(height: 12),
          Expanded(
            child: DecoratedBox(
              decoration: BoxDecoration(
                color: const Color(0xFF101513),
                borderRadius: BorderRadius.circular(8),
              ),
              child: _log.isEmpty
                  ? const Center(
                      child: Text(
                        'Sin mensajes todavia',
                        style: TextStyle(color: Color(0xFFB8C6BE)),
                      ),
                    )
                  : ListView.separated(
                      controller: _logScrollController,
                      padding: const EdgeInsets.all(12),
                      itemCount: _log.length,
                      separatorBuilder: (_, _) => const SizedBox(height: 8),
                      itemBuilder: (context, index) {
                        return _LogTile(entry: _log[index]);
                      },
                    ),
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildComposerPanel() {
    return _Panel(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              const Icon(Icons.code_outlined),
              const SizedBox(width: 8),
              Text(
                'Enviar JSON',
                style: Theme.of(context).textTheme.titleMedium,
              ),
            ],
          ),
          const SizedBox(height: 12),
          TextField(
            controller: _deviceIdController,
            decoration: const InputDecoration(
              labelText: 'device_id para comandos',
              prefixIcon: Icon(Icons.memory_outlined),
            ),
          ),
          const SizedBox(height: 12),
          Wrap(
            spacing: 8,
            runSpacing: 8,
            children: [
              _TemplateButton(
                icon: Icons.waving_hand_outlined,
                label: 'hello',
                onPressed: () => _applyTemplate(_MessageTemplate.hello),
              ),
              _TemplateButton(
                icon: Icons.network_ping_outlined,
                label: 'ping',
                onPressed: () => _applyTemplate(_MessageTemplate.ping),
              ),
              _TemplateButton(
                icon: Icons.sync_outlined,
                label: 'resync',
                onPressed: () => _applyTemplate(_MessageTemplate.resync),
              ),
              _TemplateButton(
                icon: Icons.toggle_on_outlined,
                label: 'ON',
                onPressed: () => _applyTemplate(_MessageTemplate.turnOn),
              ),
              _TemplateButton(
                icon: Icons.toggle_off_outlined,
                label: 'OFF',
                onPressed: () => _applyTemplate(_MessageTemplate.turnOff),
              ),
            ],
          ),
          const SizedBox(height: 12),
          Expanded(
            child: TextField(
              controller: _jsonController,
              expands: true,
              minLines: null,
              maxLines: null,
              textAlignVertical: TextAlignVertical.top,
              style: const TextStyle(fontFamily: 'monospace', fontSize: 13),
              decoration: const InputDecoration(
                alignLabelWithHint: true,
                labelText: 'Mensaje',
              ),
            ),
          ),
          const SizedBox(height: 12),
          Row(
            children: [
              OutlinedButton.icon(
                onPressed: _formatComposerJson,
                icon: const Icon(Icons.format_align_left_outlined),
                label: const Text('Formatear'),
              ),
              const Spacer(),
              FilledButton.icon(
                onPressed: _canSend ? _sendJson : null,
                icon: const Icon(Icons.send_outlined),
                label: const Text('Enviar'),
              ),
            ],
          ),
        ],
      ),
    );
  }
}

class _Panel extends StatelessWidget {
  const _Panel({required this.child});

  final Widget child;

  @override
  Widget build(BuildContext context) {
    return DecoratedBox(
      decoration: BoxDecoration(
        color: Theme.of(context).colorScheme.surface,
        border: Border.all(color: const Color(0xFFD8E1DD)),
        borderRadius: BorderRadius.circular(8),
      ),
      child: Padding(padding: const EdgeInsets.all(14), child: child),
    );
  }
}

class _StatusPill extends StatelessWidget {
  const _StatusPill({required this.connected, required this.connecting});

  final bool connected;
  final bool connecting;

  @override
  Widget build(BuildContext context) {
    final color = connected
        ? const Color(0xFF1E7A3E)
        : connecting
        ? const Color(0xFF956300)
        : const Color(0xFF8A1F1F);
    final label = connected
        ? 'Conectado'
        : connecting
        ? 'Conectando'
        : 'Desconectado';

    return DecoratedBox(
      decoration: BoxDecoration(
        color: color.withValues(alpha: 0.12),
        border: Border.all(color: color.withValues(alpha: 0.45)),
        borderRadius: BorderRadius.circular(999),
      ),
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
        child: Row(
          mainAxisSize: MainAxisSize.min,
          children: [
            Icon(Icons.circle, size: 10, color: color),
            const SizedBox(width: 7),
            Text(label, style: TextStyle(color: color)),
          ],
        ),
      ),
    );
  }
}

class _TemplateButton extends StatelessWidget {
  const _TemplateButton({
    required this.icon,
    required this.label,
    required this.onPressed,
  });

  final IconData icon;
  final String label;
  final VoidCallback onPressed;

  @override
  Widget build(BuildContext context) {
    return OutlinedButton.icon(
      onPressed: onPressed,
      icon: Icon(icon, size: 18),
      label: Text(label),
    );
  }
}

class _LogTile extends StatelessWidget {
  const _LogTile({required this.entry});

  final _LogEntry entry;

  @override
  Widget build(BuildContext context) {
    final color = switch (entry.direction) {
      _LogDirection.rx => const Color(0xFF79D2FF),
      _LogDirection.tx => const Color(0xFFA7E37A),
      _LogDirection.system => const Color(0xFFFFD479),
      _LogDirection.error => const Color(0xFFFF8E8E),
    };
    final label = switch (entry.direction) {
      _LogDirection.rx => 'RX',
      _LogDirection.tx => 'TX',
      _LogDirection.system => 'SYS',
      _LogDirection.error => 'ERR',
    };

    return DecoratedBox(
      decoration: BoxDecoration(
        color: const Color(0xFF18201D),
        border: Border(left: BorderSide(color: color, width: 3)),
        borderRadius: BorderRadius.circular(6),
      ),
      child: Padding(
        padding: const EdgeInsets.all(10),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Wrap(
              spacing: 8,
              runSpacing: 4,
              crossAxisAlignment: WrapCrossAlignment.center,
              children: [
                Text(
                  label,
                  style: TextStyle(
                    color: color,
                    fontWeight: FontWeight.w700,
                    letterSpacing: 0,
                  ),
                ),
                Text(
                  _clock(entry.timestamp),
                  style: const TextStyle(color: Color(0xFFB8C6BE)),
                ),
                if (entry.messageType != null)
                  Text(
                    entry.messageType!,
                    style: const TextStyle(color: Color(0xFFE3ECE7)),
                  ),
              ],
            ),
            const SizedBox(height: 6),
            SelectableText(
              entry.text,
              style: const TextStyle(
                color: Color(0xFFE3ECE7),
                fontFamily: 'monospace',
                fontSize: 12.5,
              ),
            ),
          ],
        ),
      ),
    );
  }

  String _clock(DateTime value) {
    String two(int number) => number.toString().padLeft(2, '0');
    String three(int number) => number.toString().padLeft(3, '0');
    return '${two(value.hour)}:${two(value.minute)}:${two(value.second)}.'
        '${three(value.millisecond)}';
  }
}

class _LogEntry {
  const _LogEntry({
    required this.direction,
    required this.timestamp,
    required this.text,
    required this.messageType,
  });

  final _LogDirection direction;
  final DateTime timestamp;
  final String text;
  final String? messageType;
}

enum _LogDirection { rx, tx, system, error }

enum _MessageTemplate { hello, ping, resync, turnOn, turnOff }
