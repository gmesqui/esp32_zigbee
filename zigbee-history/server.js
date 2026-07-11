import express from 'express';
import WebSocket from 'ws';
import { DatabaseSync } from 'node:sqlite';
import { mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { HistoryStore } from './history-store.js';

const __dirname = dirname(fileURLToPath(import.meta.url));
const dataDir = join(__dirname, 'data');
mkdirSync(dataDir, { recursive: true });

const defaultGatewayUrl = process.env.ZIGBEE_HISTORY_GATEWAY_URL || 'ws://esp32-zigbee.local:8080/ws';
const now = () => Date.now();
const historySubscribers = new Set();

function publishHistoryUpdate(update) {
  const message = `event: history\ndata: ${JSON.stringify(update)}\n\n`;
  for (const response of historySubscribers) response.write(message);
}

const databaseFile = process.env.ZIGBEE_HISTORY_DB || join(dataDir, 'zigbee-history.sqlite');
const db = new DatabaseSync(databaseFile);
const store = new HistoryStore(db, { onChange: publishHistoryUpdate });
const getGatewayUrl = () => store.getSetting('gatewayUrl') ?? defaultGatewayUrl;

function normalizeGatewayUrl(value) {
  const url = new URL(String(value ?? '').trim());
  if (!['ws:', 'wss:'].includes(url.protocol)) throw new Error('La URL debe comenzar por ws:// o wss://');
  return url.toString();
}

function normalizeTimestamp(value) {
  const timestamp = Number(value);
  // El firmware usa segundos desde el arranque; éstos no son una fecha real.
  if (!Number.isFinite(timestamp) || timestamp <= 0) return now();
  if (timestamp >= 1_600_000_000_000) return Math.round(timestamp);
  if (timestamp >= 1_600_000_000) return Math.round(timestamp * 1000);
  return now();
}

function acceptState(devices) {
  for (const device of devices) {
    if (!device?.device_id) continue;
    store.upsertDevice({ device_id: device.device_id, name: device.name || device.device_id });
    store.updateDeviceCaptureState(device.device_id, {
      reachable: device.meta?.reachable,
    });
    // El estado inicial sólo es una lectura válida cuando el propio puente
    // confirma que el sensor sigue accesible. Los demás snapshots quedan como
    // caché de descubrimiento, sin fingir una captura.
    const captureSnapshot = device.meta?.reachable === true;
    for (const [metric, attribute] of Object.entries(device.state || {})) {
      if (captureSnapshot) store.recordReachableSnapshot(device.device_id, metric, attribute);
      else store.recordSnapshot(device.device_id, metric, attribute);
    }
  }
}

class GatewayConnection {
  constructor() {
    this.socket = null;
    this.retryTimer = null;
    this.retryMs = 1000;
    this.status = { connected: false, url: getGatewayUrl(), error: null, lastMessageAt: null };
    this.streams = new Map();
  }

  start() {
    store.setGatewayConnected(false);
    this.connect();
  }

  reconnect() {
    this.stop();
    this.status = { ...this.status, url: getGatewayUrl(), error: null };
    this.retryMs = 1000;
    this.connect();
  }

  stop({ trackDisconnect = true } = {}) {
    clearTimeout(this.retryTimer);
    this.retryTimer = null;
    if (this.socket) {
      this.socket.removeAllListeners();
      this.socket.close();
      this.socket = null;
    }
    this.status.connected = false;
    if (trackDisconnect) store.setGatewayConnected(false);
  }

  connect() {
    const target = getGatewayUrl();
    this.status.url = target;
    let socket;
    try { socket = new WebSocket(target); } catch (error) { this.fail(error); return; }
    this.socket = socket;
    socket.on('open', () => {
      this.status = { ...this.status, connected: true, error: null };
      this.retryMs = 1000;
      store.setGatewayConnected(true);
      socket.send(JSON.stringify({ type: 'hello', msg_id: 1, require_ack: false, data: { protocol_version: 1, client: 'esp32-zigbee-history' } }));
    });
    socket.on('message', data => this.handleMessage(data.toString()));
    socket.on('error', error => { this.status.error = error.message; });
    socket.on('close', () => {
      this.status.connected = false;
      if (this.socket === socket) this.socket = null;
      store.setGatewayConnected(false);
      this.scheduleReconnect();
    });
  }

  fail(error) {
    this.status = { ...this.status, connected: false, error: error.message };
    store.setGatewayConnected(false);
    this.scheduleReconnect();
  }

  scheduleReconnect() {
    if (this.retryTimer) return;
    const delay = this.retryMs;
    this.retryMs = Math.min(this.retryMs * 2, 30_000);
    this.retryTimer = setTimeout(() => { this.retryTimer = null; this.connect(); }, delay);
  }

  acceptChunk(kind, data, complete) {
    if (!data?.stream_id || !Number.isInteger(data.index)) return;
    const key = `${kind}:${data.stream_id}`;
    const stream = this.streams.get(key) || { chunks: new Map(), finalIndex: null };
    stream.chunks.set(data.index, Array.isArray(data.devices) ? data.devices : []);
    if (data.final) stream.finalIndex = data.index;
    this.streams.set(key, stream);
    if (stream.finalIndex === null) return;
    const items = [];
    for (let index = 0; index <= stream.finalIndex; index++) {
      if (!stream.chunks.has(index)) return;
      items.push(...stream.chunks.get(index));
    }
    this.streams.delete(key);
    complete(items);
  }

  handleMessage(raw) {
    let message;
    try { message = JSON.parse(raw); } catch { return; }
    this.status.lastMessageAt = now();
    if (message.type === 'inventory_chunk') this.acceptChunk('inventory', message.data, devices => devices.forEach(device => store.upsertDevice(device)));
    if (message.type === 'state_chunk') this.acceptChunk('state', message.data, acceptState);
    if (message.type !== 'event' || !message.data?.device_id) return;

    const data = message.data;
    store.upsertDevice({ device_id: data.device_id, name: data.name || data.device_id });
    for (const [metric, attribute] of Object.entries(data.changes || {})) {
      const capturedAt = normalizeTimestamp(attribute?.ts);
      if (metric === 'reachable') {
        store.updateDeviceCaptureState(data.device_id, { reachable: Boolean(attribute?.value), timestamp: capturedAt });
      } else {
        store.recordLiveAttribute(data.device_id, metric, attribute, capturedAt);
      }
    }
  }
}

const gateway = new GatewayConnection();
const app = express();
app.use(express.json({ limit: '64kb' }));
app.use(express.static(join(__dirname, 'public')));

app.get('/api/events', (req, res) => {
  res.writeHead(200, {
    'Cache-Control': 'no-cache, no-transform',
    Connection: 'keep-alive',
    'Content-Type': 'text/event-stream',
  });
  res.write('retry: 3000\n\n');
  historySubscribers.add(res);
  req.on('close', () => historySubscribers.delete(res));
});

app.get('/api/status', (_req, res) => {
  const status = store.getStatus();
  res.json({ ...status, gateway: gateway.status, settings: { gatewayUrl: getGatewayUrl() }, serverTime: now() });
});

app.put('/api/settings', (req, res) => {
  try {
    const url = normalizeGatewayUrl(req.body.gatewayUrl);
    store.setSetting('gatewayUrl', url);
    gateway.reconnect();
    res.json({ gatewayUrl: url });
  } catch (error) {
    res.status(400).json({ error: error.message });
  }
});

app.post('/api/series', (req, res) => {
  const { deviceId, metric } = req.body;
  const label = String(req.body.label || metric || '').trim();
  if (!deviceId || !metric || !label) return res.status(400).json({ error: 'deviceId, metric y label son obligatorios.' });
  store.createOrEnableSeries(deviceId, metric, label);
  return res.status(201).json({ ok: true });
});

app.patch('/api/series/:id', (req, res) => {
  const id = Number(req.params.id);
  const label = String(req.body.label || '').trim();
  const enabled = req.body.enabled;
  if (!Number.isInteger(id) || (!label && typeof enabled !== 'boolean')) return res.status(400).json({ error: 'Cambio inválido.' });
  if (!store.updateSeries(id, { label, enabled })) return res.status(404).json({ error: 'Serie no encontrada.' });
  return res.json({ ok: true });
});

app.delete('/api/series/:id', (req, res) => {
  store.deleteSeries(Number(req.params.id));
  res.status(204).end();
});

app.get('/api/history/days', (req, res) => {
  const id = Number(req.query.seriesId);
  const from = Number(req.query.from);
  const to = Number(req.query.to);
  const timeZone = String(req.query.timeZone || 'UTC');
  if (!Number.isInteger(id)) return res.status(400).json({ error: 'seriesId inválido.' });
  if (!Number.isFinite(from) || !Number.isFinite(to) || from >= to) return res.status(400).json({ error: 'El rango de días no es válido.' });
  try {
    const days = store.getHistoryDays(id, Math.round(from), Math.round(to), timeZone);
    if (!days) return res.status(404).json({ error: 'Serie no encontrada.' });
    return res.json(days);
  } catch (error) {
    if (error instanceof RangeError) return res.status(400).json({ error: 'La zona horaria no es válida.' });
    throw error;
  }
});

app.get('/api/history', (req, res) => {
  const id = Number(req.query.seriesId);
  const hasFrom = req.query.from !== undefined;
  const hasTo = req.query.to !== undefined;
  if (hasFrom !== hasTo) return res.status(400).json({ error: 'from y to deben indicarse juntos.' });
  let from, to;
  if (hasFrom) {
    from = Number(req.query.from);
    to = Number(req.query.to);
    if (!Number.isFinite(from) || !Number.isFinite(to) || from >= to) return res.status(400).json({ error: 'El rango histórico no es válido.' });
    from = Math.round(from);
    to = Math.round(to);
  } else {
    const hours = Math.min(Math.max(Number(req.query.hours) || 24, 1), 24 * 365);
    to = now();
    from = to - hours * 3_600_000;
  }
  if (!Number.isInteger(id)) return res.status(400).json({ error: 'seriesId inválido.' });
  const history = store.getHistory(id, from, to);
  if (!history) return res.status(404).json({ error: 'Serie no encontrada.' });
  return res.json(history);
});

const port = process.env.PORT || 3090;
const server = app.listen(port, () => {
  store.startRun();
  gateway.start();
  console.log(`Histórico Zigbee en http://localhost:${port}`);
});
const housekeeping = setInterval(() => {
  store.heartbeatRun();
}, 30_000);

function shutdown() {
  clearInterval(housekeeping);
  gateway.stop({ trackDisconnect: false });
  store.stopRun();
  server.close(() => process.exit(0));
}
process.once('SIGINT', shutdown);
process.once('SIGTERM', shutdown);
