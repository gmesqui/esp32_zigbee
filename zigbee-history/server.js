import express from 'express';
import WebSocket from 'ws';
import { DatabaseSync } from 'node:sqlite';
import { mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = dirname(fileURLToPath(import.meta.url));
const dataDir = join(__dirname, 'data');
mkdirSync(dataDir, { recursive: true });
const db = new DatabaseSync(join(dataDir, 'zigbee-history.sqlite'));
db.exec(`
  PRAGMA journal_mode = WAL;
  CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT NOT NULL);
  CREATE TABLE IF NOT EXISTS devices (
    device_id TEXT PRIMARY KEY, name TEXT NOT NULL, manufacturer TEXT, model TEXT,
    capabilities_json TEXT NOT NULL DEFAULT '[]', updated_at INTEGER NOT NULL
  );
  CREATE TABLE IF NOT EXISTS device_attributes (
    device_id TEXT NOT NULL, metric TEXT NOT NULL, unit TEXT, value_type TEXT,
    last_value_json TEXT, last_seen_at INTEGER NOT NULL,
    PRIMARY KEY (device_id, metric)
  );
  CREATE TABLE IF NOT EXISTS series (
    id INTEGER PRIMARY KEY, device_id TEXT NOT NULL, metric TEXT NOT NULL,
    label TEXT NOT NULL, enabled INTEGER NOT NULL DEFAULT 1, created_at INTEGER NOT NULL,
    UNIQUE(device_id, metric)
  );
  CREATE TABLE IF NOT EXISTS readings (
    id INTEGER PRIMARY KEY, series_id INTEGER NOT NULL, captured_at INTEGER NOT NULL,
    value_json TEXT NOT NULL, numeric_value REAL, unit TEXT, source TEXT NOT NULL,
    FOREIGN KEY(series_id) REFERENCES series(id)
  );
  CREATE INDEX IF NOT EXISTS idx_readings_series_time ON readings(series_id, captured_at);
`);

const getSetting = db.prepare('SELECT value FROM settings WHERE key = ?');
const setSetting = db.prepare('INSERT INTO settings(key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value');
const defaultGatewayUrl = 'ws://esp32-zigbee.local:8080/ws';
const getGatewayUrl = () => getSetting.get('gatewayUrl')?.value ?? defaultGatewayUrl;
const now = () => Date.now();
const historySubscribers = new Set();

function publishHistoryUpdate(update) {
  const message = `event: reading\ndata: ${JSON.stringify(update)}\n\n`;
  for (const response of historySubscribers) response.write(message);
}

function normalizeGatewayUrl(value) {
  const url = new URL(String(value ?? '').trim());
  if (!['ws:', 'wss:'].includes(url.protocol)) throw new Error('La URL debe comenzar por ws:// o wss://');
  return url.toString();
}

function normalizeTimestamp(value) {
  const timestamp = Number(value);
  // El firmware puede emitir segundos desde el arranque. Sólo una fecha Unix
  // inequívoca debe usarse como reloj de la muestra; el resto se fecha al recibirla.
  if (!Number.isFinite(timestamp) || timestamp <= 0) return now();
  if (timestamp >= 1_600_000_000_000) return Math.round(timestamp);
  if (timestamp >= 1_600_000_000) return Math.round(timestamp * 1000);
  return now();
}

function numericValue(value) {
  if (typeof value === 'number' && Number.isFinite(value)) return value;
  if (typeof value === 'boolean') return value ? 1 : 0;
  return null;
}

function upsertDevice(device) {
  if (!device?.device_id) return;
  db.prepare(`INSERT INTO devices(device_id, name, manufacturer, model, capabilities_json, updated_at)
    VALUES (?, ?, ?, ?, ?, ?)
    ON CONFLICT(device_id) DO UPDATE SET name=excluded.name, manufacturer=excluded.manufacturer,
    model=excluded.model, capabilities_json=excluded.capabilities_json, updated_at=excluded.updated_at`)
    .run(device.device_id, device.name || device.device_id, device.manufacturer || null, device.model || null,
      JSON.stringify(Array.isArray(device.capabilities) ? device.capabilities : []), now());
}

function recordAttribute(deviceId, metric, attribute, source) {
  if (!deviceId || !metric || !attribute || !Object.hasOwn(attribute, 'value')) return;
  const value = attribute.value;
  const unit = typeof attribute.unit === 'string' ? attribute.unit : null;
  const capturedAt = normalizeTimestamp(attribute.ts);
  const encoded = JSON.stringify(value);
  const numeric = numericValue(value);
  db.prepare(`INSERT INTO device_attributes(device_id, metric, unit, value_type, last_value_json, last_seen_at)
    VALUES (?, ?, ?, ?, ?, ?)
    ON CONFLICT(device_id, metric) DO UPDATE SET unit=excluded.unit, value_type=excluded.value_type,
    last_value_json=excluded.last_value_json, last_seen_at=excluded.last_seen_at`)
    .run(deviceId, metric, unit, typeof value, encoded, capturedAt);

  const series = db.prepare('SELECT id FROM series WHERE device_id = ? AND metric = ? AND enabled = 1').get(deviceId, metric);
  if (!series) return;
  db.prepare('INSERT INTO readings(series_id, captured_at, value_json, numeric_value, unit, source) VALUES (?, ?, ?, ?, ?, ?)')
    .run(series.id, capturedAt, encoded, numeric, unit, source);
  publishHistoryUpdate({ seriesId: series.id, capturedAt, value, numericValue: numeric, unit, source });
}

function acceptState(devices) {
  for (const device of devices) {
    if (!device?.device_id) continue;
    upsertDevice({ device_id: device.device_id, name: device.name || device.device_id });
    for (const [metric, attribute] of Object.entries(device.state || {})) recordAttribute(device.device_id, metric, attribute, 'state_snapshot');
  }
}

class GatewayConnection {
  constructor() { this.socket = null; this.retryTimer = null; this.retryMs = 1000; this.status = { connected: false, url: getGatewayUrl(), error: null, lastMessageAt: null }; this.streams = new Map(); }
  start() { this.connect(); }
  reconnect() { this.stop(); this.status = { ...this.status, url: getGatewayUrl(), error: null }; this.retryMs = 1000; this.connect(); }
  stop() { clearTimeout(this.retryTimer); this.retryTimer = null; if (this.socket) { this.socket.removeAllListeners(); this.socket.close(); this.socket = null; } this.status.connected = false; }
  connect() {
    const target = getGatewayUrl(); this.status.url = target;
    let socket;
    try { socket = new WebSocket(target); } catch (error) { this.fail(error); return; }
    this.socket = socket;
    socket.on('open', () => { this.status = { ...this.status, connected: true, error: null }; this.retryMs = 1000; socket.send(JSON.stringify({ type: 'hello', msg_id: 1, require_ack: false, data: { protocol_version: 1, client: 'esp32-zigbee-history' } })); });
    socket.on('message', data => this.handleMessage(data.toString()));
    socket.on('error', error => { this.status.error = error.message; });
    socket.on('close', () => { this.status.connected = false; if (this.socket === socket) this.socket = null; this.scheduleReconnect(); });
  }
  fail(error) { this.status = { ...this.status, connected: false, error: error.message }; this.scheduleReconnect(); }
  scheduleReconnect() { if (this.retryTimer) return; const delay = this.retryMs; this.retryMs = Math.min(this.retryMs * 2, 30_000); this.retryTimer = setTimeout(() => { this.retryTimer = null; this.connect(); }, delay); }
  acceptChunk(kind, data, complete) {
    if (!data?.stream_id || !Number.isInteger(data.index)) return;
    const key = `${kind}:${data.stream_id}`; const stream = this.streams.get(key) || { chunks: new Map(), finalIndex: null };
    stream.chunks.set(data.index, Array.isArray(data.devices) ? data.devices : []); if (data.final) stream.finalIndex = data.index; this.streams.set(key, stream);
    if (stream.finalIndex === null) return;
    const items = [];
    for (let index = 0; index <= stream.finalIndex; index++) { if (!stream.chunks.has(index)) return; items.push(...stream.chunks.get(index)); }
    this.streams.delete(key); complete(items);
  }
  handleMessage(raw) {
    let message; try { message = JSON.parse(raw); } catch { return; }
    this.status.lastMessageAt = now();
    if (message.type === 'inventory_chunk') this.acceptChunk('inventory', message.data, devices => devices.forEach(upsertDevice));
    if (message.type === 'state_chunk') this.acceptChunk('state', message.data, acceptState);
    if (message.type === 'event' && message.data?.device_id) {
      const data = message.data; upsertDevice({ device_id: data.device_id, name: data.name || data.device_id });
      for (const [metric, attribute] of Object.entries(data.changes || {})) if (metric !== 'reachable') recordAttribute(data.device_id, metric, attribute, 'event');
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
    'Connection': 'keep-alive',
    'Content-Type': 'text/event-stream',
  });
  res.write('retry: 3000\n\n');
  historySubscribers.add(res);
  req.on('close', () => historySubscribers.delete(res));
});

app.get('/api/status', (_req, res) => {
  const devices = db.prepare(`SELECT d.device_id, d.name, d.manufacturer, d.model, d.capabilities_json, d.updated_at,
    (SELECT COUNT(*) FROM series s WHERE s.device_id=d.device_id AND s.enabled=1) AS tracked_count
    FROM devices d
    UNION
    SELECT a.device_id, a.device_id AS name, NULL AS manufacturer, NULL AS model, '[]' AS capabilities_json, a.last_seen_at AS updated_at,
    (SELECT COUNT(*) FROM series s WHERE s.device_id=a.device_id AND s.enabled=1) AS tracked_count
    FROM device_attributes a WHERE NOT EXISTS (SELECT 1 FROM devices d WHERE d.device_id=a.device_id)
    ORDER BY name COLLATE NOCASE`).all().map(row => ({ ...row, capabilities: JSON.parse(row.capabilities_json) }));
  const attributes = db.prepare('SELECT device_id, metric, unit, value_type, last_value_json, last_seen_at FROM device_attributes ORDER BY metric').all()
    .map(row => ({ ...row, last_value: JSON.parse(row.last_value_json) }));
  const series = db.prepare(`SELECT s.*, d.name AS device_name, a.unit, a.last_value_json, a.last_seen_at,
    (SELECT COUNT(*) FROM readings r WHERE r.series_id=s.id) AS reading_count FROM series s
    LEFT JOIN devices d ON d.device_id=s.device_id LEFT JOIN device_attributes a ON a.device_id=s.device_id AND a.metric=s.metric
    ORDER BY d.name COLLATE NOCASE, s.metric`).all().map(row => ({ ...row, last_value: row.last_value_json ? JSON.parse(row.last_value_json) : null }));
  res.json({ gateway: gateway.status, settings: { gatewayUrl: getGatewayUrl() }, devices, attributes, series, serverTime: now() });
});

app.put('/api/settings', (req, res) => {
  try { const url = normalizeGatewayUrl(req.body.gatewayUrl); setSetting.run('gatewayUrl', url); gateway.reconnect(); res.json({ gatewayUrl: url }); }
  catch (error) { res.status(400).json({ error: error.message }); }
});

app.post('/api/series', (req, res) => {
  const { deviceId, metric } = req.body; const label = String(req.body.label || metric || '').trim();
  if (!deviceId || !metric || !label) return res.status(400).json({ error: 'deviceId, metric y label son obligatorios.' });
  db.prepare(`INSERT INTO series(device_id, metric, label, enabled, created_at) VALUES (?, ?, ?, 1, ?)
    ON CONFLICT(device_id, metric) DO UPDATE SET label=excluded.label, enabled=1`).run(String(deviceId), String(metric), label, now());
  res.status(201).json({ ok: true });
});

app.patch('/api/series/:id', (req, res) => {
  const id = Number(req.params.id); const label = String(req.body.label || '').trim(); const enabled = req.body.enabled;
  if (!Number.isInteger(id) || (!label && typeof enabled !== 'boolean')) return res.status(400).json({ error: 'Cambio inválido.' });
  if (label) db.prepare('UPDATE series SET label = ? WHERE id = ?').run(label, id);
  if (typeof enabled === 'boolean') db.prepare('UPDATE series SET enabled = ? WHERE id = ?').run(enabled ? 1 : 0, id);
  res.json({ ok: true });
});

app.delete('/api/series/:id', (req, res) => { const id = Number(req.params.id); db.prepare('DELETE FROM readings WHERE series_id = ?').run(id); db.prepare('DELETE FROM series WHERE id = ?').run(id); res.status(204).end(); });

app.get('/api/history', (req, res) => {
  const id = Number(req.query.seriesId); const hours = Math.min(Math.max(Number(req.query.hours) || 24, 1), 24 * 365);
  if (!Number.isInteger(id)) return res.status(400).json({ error: 'seriesId inválido.' });
  const from = now() - hours * 3_600_000;
  const series = db.prepare('SELECT * FROM series WHERE id = ?').get(id); if (!series) return res.status(404).json({ error: 'Serie no encontrada.' });
  const readings = db.prepare('SELECT captured_at, value_json, numeric_value, unit, source FROM readings WHERE series_id = ? AND captured_at >= ? ORDER BY captured_at').all(id, from)
    .map(row => ({ ...row, value: JSON.parse(row.value_json) }));
  const readingCount = db.prepare('SELECT COUNT(*) AS count FROM readings WHERE series_id = ?').get(id).count;
  res.json({ series, from, to: now(), readings, readingCount });
});

app.listen(process.env.PORT || 3090, () => { console.log(`Histórico Zigbee en http://localhost:${process.env.PORT || 3090}`); gateway.start(); });
