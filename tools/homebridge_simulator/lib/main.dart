import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:web_socket_channel/status.dart' as ws_status;
import 'package:web_socket_channel/web_socket_channel.dart';

void main() {
  runApp(const HomebridgeSimulatorApp());
}

const _defaultMdnsName = 'esp32-zigbee.local';
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
  final Map<String, _InventoryDevice> _inventoryById = {};
  final Map<String, _StateDeviceSnapshot> _stateById = {};
  final Map<String, _ChunkAccumulator<_InventoryDevice>> _inventoryStreams = {};
  final Map<String, _ChunkAccumulator<_StateDeviceSnapshot>> _stateStreams = {};
  final Map<int, _PendingDeviceCommand> _pendingCommandsByMsgId = {};
  final Map<String, _PendingDeviceCommand> _pendingCommandsByDeviceId = {};

  var _nextMsgId = 1;
  var _useTls = false;
  var _sendHelloOnConnect = false;
  var _connecting = false;
  var _connected = false;

  bool get _canSend => _connected && _channel != null;

  List<_TrackedDevice> get _trackedDevices {
    final deviceIds = <String>{
      ..._inventoryById.keys,
      ..._stateById.keys,
    }.toList()
      ..sort((left, right) {
        final leftName =
            (_inventoryById[left]?.name ?? left).toLowerCase();
        final rightName =
            (_inventoryById[right]?.name ?? right).toLowerCase();
        final nameCompare = leftName.compareTo(rightName);
        if (nameCompare != 0) {
          return nameCompare;
        }
        return left.compareTo(right);
      });

    return deviceIds
        .map(
          (deviceId) => _TrackedDevice(
            deviceId: deviceId,
            inventory: _inventoryById[deviceId],
            snapshot: _stateById[deviceId],
            pendingCommand: _pendingCommandsByDeviceId[deviceId],
          ),
        )
        .toList(growable: false);
  }

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
      setState(() {
        _connected = false;
        _clearPendingCommands();
      });
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
      _clearPendingCommands();
    });
    _addLog(_LogDirection.system, 'Conexion cerrada por el ESP32');
  }

  void _handleIncoming(dynamic message) {
    final text = message?.toString() ?? '';
    _addLog(_LogDirection.rx, text);
    _processIncoming(text);
  }

  void _processIncoming(String rawText) {
    final message = _asStringKeyedMap(_decodeJsonSafely(rawText));
    if (message == null) {
      return;
    }

    final type = _asString(message['type']);
    final data = _asStringKeyedMap(message['data']) ?? const <String, dynamic>{};

    switch (type) {
      case 'inventory_chunk':
        _acceptInventoryChunk(data);
        break;
      case 'state_chunk':
        _acceptStateChunk(data);
        break;
      case 'event':
      case 'device_joined':
      case 'device_updated':
      case 'device_left':
        _acceptDeviceEvent(type!, data);
        break;
      case 'cmd_result':
        _handleCmdResult(message);
        break;
      case 'error':
        _handleCmdError(message);
        break;
      default:
        break;
    }
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

    Object? decoded;
    try {
      decoded = jsonDecode(text);
    } on FormatException catch (error) {
      _addLog(_LogDirection.error, 'JSON invalido: ${error.message}');
      return;
    }

    _trackOutgoingCommand(decoded);
    _channel!.sink.add(text);
    _addLog(_LogDirection.tx, text);
  }

  void _sendObject(Map<String, dynamic> message) {
    _trackOutgoingCommand(message);
    final text = _pretty(message);
    _channel?.sink.add(text);
    _addLog(_LogDirection.tx, text);
  }

  void _sendDeviceStateCommand(String deviceId, bool state) {
    if (!_canSend) {
      _addLog(_LogDirection.error, 'No hay conexion activa');
      return;
    }

    _deviceIdController.text = deviceId;
    _sendObject(_commandMessage(deviceId, state));
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

  void _acceptInventoryChunk(Map<String, dynamic> data) {
    _acceptChunk<_InventoryDevice>(
      streams: _inventoryStreams,
      data: data,
      itemParser: (item) => _InventoryDevice.fromJson(item),
      onComplete: (devices) {
        _inventoryById
          ..clear()
          ..addEntries(
            devices.map((device) => MapEntry(device.deviceId, device)),
          );
        _stateById.removeWhere((deviceId, _) => !_inventoryById.containsKey(deviceId));
        _dropPendingForUnknownDevices();
      },
    );
  }

  void _acceptStateChunk(Map<String, dynamic> data) {
    _acceptChunk<_StateDeviceSnapshot>(
      streams: _stateStreams,
      data: data,
      itemParser: (item) => _StateDeviceSnapshot.fromJson(item),
      onComplete: (devices) {
        _stateById
          ..clear()
          ..addEntries(
            devices.map((device) => MapEntry(device.deviceId, device)),
          );
      },
    );
  }

  void _acceptChunk<T>({
    required Map<String, _ChunkAccumulator<T>> streams,
    required Map<String, dynamic> data,
    required T? Function(Map<String, dynamic> item) itemParser,
    required void Function(List<T> items) onComplete,
  }) {
    final streamId = _asString(data['stream_id']);
    final generation = _asInt(data['generation']);
    final index = _asInt(data['index']);
    final isFinal = data['final'] == true;
    if (streamId == null || generation == null || index == null) {
      return;
    }

    final rawItems = data['devices'];
    final items = rawItems is List
        ? rawItems
            .map((item) => _asStringKeyedMap(item))
            .whereType<Map<String, dynamic>>()
            .map(itemParser)
            .whereType<T>()
            .toList(growable: false)
        : <T>[];

    setState(() {
      final accumulator =
          streams[streamId] ?? _ChunkAccumulator<T>(generation: generation);
      accumulator.generation = generation;
      accumulator.chunks[index] = items;
      if (isFinal) {
        accumulator.finalIndex = index;
      }
      streams[streamId] = accumulator;

      final finalIndex = accumulator.finalIndex;
      if (finalIndex == null) {
        return;
      }

      for (var chunkIndex = 0; chunkIndex <= finalIndex; chunkIndex++) {
        if (!accumulator.chunks.containsKey(chunkIndex)) {
          return;
        }
      }

      final mergedItems = <T>[];
      for (var chunkIndex = 0; chunkIndex <= finalIndex; chunkIndex++) {
        final chunkItems = accumulator.chunks[chunkIndex];
        if (chunkItems != null) {
          mergedItems.addAll(chunkItems);
        }
      }

      streams.remove(streamId);
      onComplete(mergedItems);
    });
  }

  void _acceptDeviceEvent(String type, Map<String, dynamic> data) {
    final deviceId = _asString(data['device_id']);

    setState(() {
      if (deviceId != null && deviceId.isNotEmpty) {
        final existingInventory = _inventoryById[deviceId];
        final existingState = _stateById[deviceId] ??
            _StateDeviceSnapshot.empty(deviceId);

        if (type == 'device_joined' || type == 'device_updated') {
          final incomingName = _asString(data['name']);
          _inventoryById[deviceId] = (existingInventory ??
                  _InventoryDevice.placeholder(deviceId))
              .copyWith(name: incomingName);
        }

        if (type == 'device_left') {
          _stateById[deviceId] = existingState.copyWith(
            meta: existingState.meta.copyWith(
              reachable: false,
              lastSeen: _protocolNow(),
            ),
          );
        }

        if (type == 'device_joined') {
          _stateById[deviceId] = existingState.copyWith(
            meta: existingState.meta.copyWith(
              reachable: true,
              lastSeen: _protocolNow(),
            ),
          );
        }

        if (type == 'event') {
          _stateById[deviceId] = _applyChangesToSnapshot(existingState, data);
        }
      }
    });
  }

  _StateDeviceSnapshot _applyChangesToSnapshot(
    _StateDeviceSnapshot snapshot,
    Map<String, dynamic> data,
  ) {
    var meta = snapshot.meta.copyWith(lastSeen: _protocolNow());
    final nextState = Map<String, _AttributeValue>.from(snapshot.state);
    final changes = _asStringKeyedMap(data['changes']);
    if (changes != null) {
      for (final entry in changes.entries) {
        final value = _AttributeValue.fromJson(_asStringKeyedMap(entry.value));
        if (entry.key == 'reachable') {
          meta = meta.copyWith(reachable: value.boolValue);
        } else {
          nextState[entry.key] = value;
        }
      }
    }

    return snapshot.copyWith(meta: meta, state: nextState);
  }

  void _trackOutgoingCommand(Object? message) {
    final json = _asStringKeyedMap(message);
    final type = _asString(json?['type']);
    if (json == null || type != 'cmd') {
      return;
    }

    final msgId = _asInt(json['msg_id']);
    final data = _asStringKeyedMap(json['data']);
    final params = _asStringKeyedMap(data?['params']);
    final deviceId = _asString(data?['device_id']);
    final cluster = _asString(data?['cluster']);
    final command = _asString(data?['command']);
    final state = params?['state'];

    if (msgId == null ||
        deviceId == null ||
        cluster != 'onoff' ||
        command != 'set' ||
        state is! bool) {
      return;
    }

    final pending = _PendingDeviceCommand(
      msgId: msgId,
      deviceId: deviceId,
      desiredState: state,
      previousState: _stateById[deviceId]?.state['state']?.boolValue,
    );

    setState(() {
      _removePendingForDevice(deviceId);
      _pendingCommandsByMsgId[msgId] = pending;
      _pendingCommandsByDeviceId[deviceId] = pending;
    });
  }

  void _handleCmdResult(Map<String, dynamic> message) {
    final replyTo = _asInt(message['reply_to']);
    final data = _asStringKeyedMap(message['data']);

    if (replyTo == null) {
      return;
    }

    setState(() {
      final pending = _takePendingCommand(replyTo);
      if (pending == null || data == null) {
        return;
      }

      final status = _asString(data['status']);
      final applied = data['applied'] == true;
      if (status == 'ok' && applied) {
        final snapshot = _stateById[pending.deviceId] ??
            _StateDeviceSnapshot.empty(pending.deviceId);
        _stateById[pending.deviceId] = snapshot.copyWith(
          meta: snapshot.meta.copyWith(lastSeen: _protocolNow()),
          state: {
            ...snapshot.state,
            'state': _AttributeValue(
              value: pending.desiredState ? 'ON' : 'OFF',
              ts: _protocolNow(),
              quality: 'valid',
            ),
          },
        );
      }
    });
  }

  void _handleCmdError(Map<String, dynamic> message) {
    final replyTo = _asInt(message['reply_to']);
    if (replyTo == null) {
      return;
    }

    setState(() {
      _takePendingCommand(replyTo);
    });
  }

  _PendingDeviceCommand? _takePendingCommand(int msgId) {
    final pending = _pendingCommandsByMsgId.remove(msgId);
    if (pending == null) {
      return null;
    }

    final current = _pendingCommandsByDeviceId[pending.deviceId];
    if (current?.msgId == msgId) {
      _pendingCommandsByDeviceId.remove(pending.deviceId);
    }
    return pending;
  }

  void _removePendingForDevice(String deviceId) {
    final existing = _pendingCommandsByDeviceId.remove(deviceId);
    if (existing != null) {
      _pendingCommandsByMsgId.remove(existing.msgId);
    }
  }

  void _dropPendingForUnknownDevices() {
    final validIds = <String>{..._inventoryById.keys, ..._stateById.keys};
    _pendingCommandsByDeviceId.removeWhere((deviceId, _) => !validIds.contains(deviceId));
    _pendingCommandsByMsgId.removeWhere(
      (_, pending) => !validIds.contains(pending.deviceId),
    );
  }

  void _clearPendingCommands() {
    _pendingCommandsByMsgId.clear();
    _pendingCommandsByDeviceId.clear();
  }

  void _loadDeviceIntoComposer(String deviceId) {
    setState(() {
      _deviceIdController.text = deviceId;
    });
  }

  int _protocolNow() => DateTime.now().millisecondsSinceEpoch ~/ 1000;

  Object? _decodeJsonSafely(String rawText) {
    try {
      return jsonDecode(rawText);
    } on Object {
      return null;
    }
  }

  Map<String, dynamic>? _asStringKeyedMap(Object? value) {
    if (value is Map) {
      return value.map(
        (key, item) => MapEntry(key.toString(), item),
      );
    }
    return null;
  }

  int? _asInt(Object? value) {
    if (value is int) {
      return value;
    }
    if (value is num) {
      return value.toInt();
    }
    return null;
  }

  String? _asString(Object? value) {
    return value is String ? value : null;
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
            final desktop = constraints.maxWidth >= 960;

            return Padding(
              padding: const EdgeInsets.all(16),
              child: Column(
                children: [
                  _buildConnectionPanel(),
                  const SizedBox(height: 16),
                  Expanded(
                    child: desktop
                        ? _buildDesktopContent()
                        : _buildMobileContent(),
                  ),
                ],
              ),
            );
          },
        ),
      ),
    );
  }

  Widget _buildDesktopContent() {
    return Row(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Expanded(flex: 5, child: _buildLogPanel()),
        const SizedBox(width: 16),
        Expanded(
          flex: 6,
          child: LayoutBuilder(
            builder: (context, constraints) {
              final splitRight = constraints.maxWidth >= 760;
              if (splitRight) {
                return Row(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    Expanded(flex: 5, child: _buildComposerPanel()),
                    const SizedBox(width: 16),
                    Expanded(flex: 4, child: _buildDevicesPanel()),
                  ],
                );
              }

              return Column(
                children: [
                  Expanded(flex: 6, child: _buildComposerPanel()),
                  const SizedBox(height: 16),
                  Expanded(flex: 5, child: _buildDevicesPanel()),
                ],
              );
            },
          ),
        ),
      ],
    );
  }

  Widget _buildMobileContent() {
    return SingleChildScrollView(
      child: Column(
        children: [
          SizedBox(height: 320, child: _buildLogPanel()),
          const SizedBox(height: 16),
          SizedBox(height: 560, child: _buildComposerPanel()),
          const SizedBox(height: 16),
          SizedBox(height: 560, child: _buildDevicesPanel()),
        ],
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

  Widget _buildDevicesPanel() {
    final devices = _trackedDevices;

    return _Panel(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              const Icon(Icons.devices_other_outlined),
              const SizedBox(width: 8),
              Text(
                'Dispositivos',
                style: Theme.of(context).textTheme.titleMedium,
              ),
              const Spacer(),
              Text(
                '${devices.length}',
                style: Theme.of(context).textTheme.labelLarge,
              ),
            ],
          ),
          const SizedBox(height: 6),
          Text(
            'Inventario y estado en tiempo real a partir de inventory/state/event.',
            style: Theme.of(context).textTheme.bodySmall?.copyWith(
              color: Theme.of(context).colorScheme.onSurfaceVariant,
            ),
          ),
          const SizedBox(height: 12),
          Expanded(
            child: devices.isEmpty
                ? Center(
                    child: Text(
                      'Aun no se han recibido dispositivos',
                      style: Theme.of(context).textTheme.bodyMedium?.copyWith(
                        color: Theme.of(context).colorScheme.onSurfaceVariant,
                      ),
                    ),
                  )
                : ListView.separated(
                    itemCount: devices.length,
                    separatorBuilder: (_, _) => const SizedBox(height: 10),
                    itemBuilder: (context, index) {
                      final device = devices[index];
                      return _DeviceTile(
                        device: device,
                        canSend: _canSend,
                        onUseDeviceId: () => _loadDeviceIntoComposer(device.deviceId),
                        onToggleSwitch: device.supportsSwitch
                            ? (value) => _sendDeviceStateCommand(
                                  device.deviceId,
                                  value,
                                )
                            : null,
                      );
                    },
                  ),
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

class _DeviceTile extends StatelessWidget {
  const _DeviceTile({
    required this.device,
    required this.canSend,
    required this.onUseDeviceId,
    required this.onToggleSwitch,
  });

  final _TrackedDevice device;
  final bool canSend;
  final VoidCallback onUseDeviceId;
  final ValueChanged<bool>? onToggleSwitch;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);

    return DecoratedBox(
      decoration: BoxDecoration(
        color: const Color(0xFFF9FBFA),
        border: Border.all(color: const Color(0xFFD8E1DD)),
        borderRadius: BorderRadius.circular(8),
      ),
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(
                        device.name,
                        style: theme.textTheme.titleSmall?.copyWith(
                          fontWeight: FontWeight.w700,
                        ),
                      ),
                      const SizedBox(height: 4),
                      Wrap(
                        spacing: 6,
                        runSpacing: 6,
                        children: [
                          _Badge(
                            label: device.reachableLabel,
                            backgroundColor: device.isReachable == true
                                ? const Color(0xFFE3F4E8)
                                : const Color(0xFFF7E6E6),
                            foregroundColor: device.isReachable == true
                                ? const Color(0xFF1E7A3E)
                                : const Color(0xFF8A1F1F),
                          ),
                          if (device.pendingCommand != null)
                            const _Badge(
                              label: 'Pendiente',
                              backgroundColor: Color(0xFFFFF3DA),
                              foregroundColor: Color(0xFF8A5A00),
                            ),
                        ],
                      ),
                    ],
                  ),
                ),
                IconButton(
                  onPressed: onUseDeviceId,
                  tooltip: 'Usar device_id en el composer',
                  icon: const Icon(Icons.keyboard_command_key_outlined),
                ),
              ],
            ),
            const SizedBox(height: 6),
            Text(
              device.subtitle,
              style: theme.textTheme.bodySmall?.copyWith(
                color: theme.colorScheme.onSurfaceVariant,
              ),
            ),
            const SizedBox(height: 4),
            SelectableText(
              device.deviceId,
              style: const TextStyle(
                fontFamily: 'monospace',
                fontSize: 12.5,
              ),
            ),
            if (device.capabilities.isNotEmpty) ...[
              const SizedBox(height: 8),
              Wrap(
                spacing: 6,
                runSpacing: 6,
                children: device.capabilities
                    .map((capability) => Chip(label: Text(capability)))
                    .toList(growable: false),
              ),
            ],
            if (device.metaSummary.isNotEmpty) ...[
              const SizedBox(height: 8),
              Text(
                device.metaSummary,
                style: theme.textTheme.bodySmall?.copyWith(
                  color: theme.colorScheme.onSurfaceVariant,
                ),
              ),
            ],
            if (device.supportsSwitch) ...[
              const SizedBox(height: 10),
              Row(
                children: [
                  Expanded(
                    child: Text(
                      'Switch on/off',
                      style: theme.textTheme.bodyMedium,
                    ),
                  ),
                  Text(
                    device.switchValue == true ? 'ON' : 'OFF',
                    style: theme.textTheme.labelLarge,
                  ),
                  const SizedBox(width: 8),
                  if (device.pendingCommand != null)
                    const SizedBox(
                      width: 18,
                      height: 18,
                      child: CircularProgressIndicator(strokeWidth: 2.2),
                    ),
                  if (device.pendingCommand != null) const SizedBox(width: 10),
                  Switch.adaptive(
                    value: device.switchValue ?? false,
                    onChanged: canSend && onToggleSwitch != null
                        ? onToggleSwitch
                        : null,
                  ),
                ],
              ),
            ],
            if (device.stateEntries.isNotEmpty) ...[
              const SizedBox(height: 8),
              Wrap(
                spacing: 8,
                runSpacing: 8,
                children: device.stateEntries
                    .map((entry) => _StateChip(name: entry.key, value: entry.value))
                    .toList(growable: false),
              ),
            ],
          ],
        ),
      ),
    );
  }
}

class _Badge extends StatelessWidget {
  const _Badge({
    required this.label,
    required this.backgroundColor,
    required this.foregroundColor,
  });

  final String label;
  final Color backgroundColor;
  final Color foregroundColor;

  @override
  Widget build(BuildContext context) {
    return DecoratedBox(
      decoration: BoxDecoration(
        color: backgroundColor,
        borderRadius: BorderRadius.circular(999),
      ),
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 5),
        child: Text(
          label,
          style: TextStyle(
            color: foregroundColor,
            fontWeight: FontWeight.w600,
          ),
        ),
      ),
    );
  }
}

class _StateChip extends StatelessWidget {
  const _StateChip({required this.name, required this.value});

  final String name;
  final _AttributeValue value;

  @override
  Widget build(BuildContext context) {
    return DecoratedBox(
      decoration: BoxDecoration(
        color: const Color(0xFFEFF4F2),
        borderRadius: BorderRadius.circular(8),
        border: Border.all(color: const Color(0xFFD8E1DD)),
      ),
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 8),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          mainAxisSize: MainAxisSize.min,
          children: [
            Text(
              name,
              style: Theme.of(context).textTheme.labelMedium?.copyWith(
                    color: Theme.of(context).colorScheme.onSurfaceVariant,
                  ),
            ),
            const SizedBox(height: 2),
            Text(
              value.displayValue,
              style: Theme.of(context).textTheme.bodyMedium?.copyWith(
                    fontWeight: FontWeight.w600,
                  ),
            ),
          ],
        ),
      ),
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

class _ChunkAccumulator<T> {
  _ChunkAccumulator({required this.generation});

  int generation;
  final Map<int, List<T>> chunks = {};
  int? finalIndex;
}

class _InventoryDevice {
  const _InventoryDevice({
    required this.deviceId,
    required this.name,
    required this.manufacturer,
    required this.model,
    required this.powerSource,
    required this.capabilities,
  });

  factory _InventoryDevice.fromJson(Map<String, dynamic> json) {
    final deviceId = json['device_id'];
    if (deviceId is! String || deviceId.isEmpty) {
      return _InventoryDevice.placeholder('unknown-device');
    }

    final capabilities = json['capabilities'] is List
        ? (json['capabilities'] as List)
            .map((item) => item.toString())
            .where((item) => item.isNotEmpty)
            .toList(growable: false)
        : const <String>[];

    return _InventoryDevice(
      deviceId: deviceId,
      name: (json['name'] as String?)?.trim().isNotEmpty == true
          ? (json['name'] as String).trim()
          : deviceId,
      manufacturer: (json['manufacturer'] as String?)?.trim() ?? 'Unknown',
      model: (json['model'] as String?)?.trim() ?? 'Unknown',
      powerSource: (json['power_source'] as String?)?.trim() ?? 'unknown',
      capabilities: capabilities,
    );
  }

  factory _InventoryDevice.placeholder(String deviceId) {
    return _InventoryDevice(
      deviceId: deviceId,
      name: deviceId,
      manufacturer: 'Unknown',
      model: 'Unknown',
      powerSource: 'unknown',
      capabilities: const [],
    );
  }

  final String deviceId;
  final String name;
  final String manufacturer;
  final String model;
  final String powerSource;
  final List<String> capabilities;

  _InventoryDevice copyWith({String? name}) {
    return _InventoryDevice(
      deviceId: deviceId,
      name: (name?.trim().isNotEmpty == true) ? name!.trim() : this.name,
      manufacturer: manufacturer,
      model: model,
      powerSource: powerSource,
      capabilities: capabilities,
    );
  }
}

class _StateDeviceSnapshot {
  const _StateDeviceSnapshot({
    required this.deviceId,
    required this.meta,
    required this.state,
  });

  factory _StateDeviceSnapshot.empty(String deviceId) {
    return _StateDeviceSnapshot(
      deviceId: deviceId,
      meta: const _DeviceMetaState(),
      state: const {},
    );
  }

  factory _StateDeviceSnapshot.fromJson(Map<String, dynamic> json) {
    final deviceId = json['device_id'];
    if (deviceId is! String || deviceId.isEmpty) {
      return _StateDeviceSnapshot.empty('unknown-device');
    }

    final rawState = json['state'];
    final state = <String, _AttributeValue>{};
    if (rawState is Map) {
      for (final entry in rawState.entries) {
        state[entry.key.toString()] = _AttributeValue.fromJson(
          entry.value is Map
              ? entry.value.map(
                  (key, value) => MapEntry(key.toString(), value),
                )
              : null,
        );
      }
    }

    return _StateDeviceSnapshot(
      deviceId: deviceId,
      meta: _DeviceMetaState.fromJson(
        json['meta'] is Map
            ? (json['meta'] as Map).map(
                (key, value) => MapEntry(key.toString(), value),
              )
            : null,
      ),
      state: state,
    );
  }

  final String deviceId;
  final _DeviceMetaState meta;
  final Map<String, _AttributeValue> state;

  _StateDeviceSnapshot copyWith({
    _DeviceMetaState? meta,
    Map<String, _AttributeValue>? state,
  }) {
    return _StateDeviceSnapshot(
      deviceId: deviceId,
      meta: meta ?? this.meta,
      state: state ?? this.state,
    );
  }
}

class _DeviceMetaState {
  const _DeviceMetaState({
    this.reachable,
    this.lastSeen,
    this.linkQuality,
  });

  factory _DeviceMetaState.fromJson(Map<String, dynamic>? json) {
    if (json == null) {
      return const _DeviceMetaState();
    }

    return _DeviceMetaState(
      reachable: json['reachable'] is bool ? json['reachable'] as bool : null,
      lastSeen: json['last_seen'] is num ? (json['last_seen'] as num).toInt() : null,
      linkQuality: json['link_quality'] is num
          ? (json['link_quality'] as num).toInt()
          : null,
    );
  }

  final bool? reachable;
  final int? lastSeen;
  final int? linkQuality;

  _DeviceMetaState copyWith({
    bool? reachable,
    int? lastSeen,
    int? linkQuality,
  }) {
    return _DeviceMetaState(
      reachable: reachable ?? this.reachable,
      lastSeen: lastSeen ?? this.lastSeen,
      linkQuality: linkQuality ?? this.linkQuality,
    );
  }
}

class _AttributeValue {
  const _AttributeValue({
    required this.value,
    this.unit,
    this.ts,
    this.quality,
  });

  factory _AttributeValue.fromJson(Map<String, dynamic>? json) {
    if (json == null) {
      return const _AttributeValue(value: null);
    }

    return _AttributeValue(
      value: json['value'],
      unit: json['unit'] as String?,
      ts: json['ts'] is num ? (json['ts'] as num).toInt() : null,
      quality: json['quality'] as String?,
    );
  }

  final Object? value;
  final String? unit;
  final int? ts;
  final String? quality;

  bool? get boolValue {
    final current = value;
    if (current is bool) {
      return current;
    }
    if (current is String) {
      if (current.toUpperCase() == 'ON') {
        return true;
      }
      if (current.toUpperCase() == 'OFF') {
        return false;
      }
    }
    if (current is num) {
      return current != 0;
    }
    return null;
  }

  String get displayValue {
    final current = value;
    final rendered = switch (current) {
      null => 'sin dato',
      final bool flag => flag ? 'true' : 'false',
      _ => current.toString(),
    };

    if (unit == null || unit!.isEmpty) {
      return rendered;
    }
    return '$rendered $unit';
  }
}

class _PendingDeviceCommand {
  const _PendingDeviceCommand({
    required this.msgId,
    required this.deviceId,
    required this.desiredState,
    required this.previousState,
  });

  final int msgId;
  final String deviceId;
  final bool desiredState;
  final bool? previousState;
}

class _TrackedDevice {
  const _TrackedDevice({
    required this.deviceId,
    required this.inventory,
    required this.snapshot,
    required this.pendingCommand,
  });

  final String deviceId;
  final _InventoryDevice? inventory;
  final _StateDeviceSnapshot? snapshot;
  final _PendingDeviceCommand? pendingCommand;

  String get name => inventory?.name ?? deviceId;

  String get subtitle =>
      '${inventory?.manufacturer ?? 'Unknown'} · ${inventory?.model ?? 'Unknown'} · '
      '${inventory?.powerSource ?? 'unknown'}';

  List<String> get capabilities => inventory?.capabilities ?? const [];

  bool get supportsSwitch => capabilities.contains('switch');

  bool? get isReachable => snapshot?.meta.reachable;

  String get reachableLabel => switch (snapshot?.meta.reachable) {
        true => 'Online',
        false => 'Offline',
        null => 'Sin reachability',
      };

  bool? get switchValue =>
      pendingCommand?.desiredState ?? snapshot?.state['state']?.boolValue;

  String get metaSummary {
    final parts = <String>[];
    final lastSeen = snapshot?.meta.lastSeen;
    final lqi = snapshot?.meta.linkQuality;
    if (lastSeen != null) {
      parts.add('last_seen: ${lastSeen}s');
    }
    if (lqi != null) {
      parts.add('lqi: $lqi');
    }
    return parts.join(' · ');
  }

  List<MapEntry<String, _AttributeValue>> get stateEntries {
    final entries = snapshot?.state.entries.toList() ?? const [];
    entries.sort((left, right) => left.key.compareTo(right.key));
    return entries;
  }
}

enum _LogDirection { rx, tx, system, error }

enum _MessageTemplate { hello, ping, resync, turnOn, turnOff }
