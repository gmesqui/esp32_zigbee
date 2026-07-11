import { DatabaseSync } from 'node:sqlite';

const CONFIRMED_READING_SOURCES = "('event', 'reachable_snapshot')";

const GAP_REASONS = new Set([
  'collector_stopped',
  'gateway_disconnected',
  'sensor_unreachable',
  'awaiting_first_sample',
  'series_paused',
  'legacy_snapshot',
]);

function numericValue(value) {
  if (typeof value === 'number' && Number.isFinite(value)) return value;
  if (typeof value === 'boolean') return value ? 1 : 0;
  return null;
}

function decodeReading(row) {
  return row ? { ...row, value: JSON.parse(row.value_json) } : null;
}

function dayFormatter(timeZone) {
  return new Intl.DateTimeFormat('en-CA', {
    timeZone,
    year: 'numeric',
    month: '2-digit',
    day: '2-digit',
  });
}

function formattedDay(formatter, timestamp) {
  const parts = Object.fromEntries(formatter.formatToParts(timestamp)
    .filter(part => part.type !== 'literal')
    .map(part => [part.type, part.value]));
  return `${parts.year}-${parts.month}-${parts.day}`;
}

/**
 * SQLite-backed capture history. A live event is confirmed evidence, as is
 * the initial state only when the bridge explicitly marks the device reachable.
 * Other snapshots remain discovery cache and are never promoted to readings.
 */
export class HistoryStore {
  constructor(db, { clock = () => Date.now(), onChange = null } = {}) {
    this.db = db;
    this.clock = clock;
    this.onChange = onChange;
    this.runId = null;
    this.initialize();
  }

  static open(filename, options = {}) {
    return new HistoryStore(new DatabaseSync(filename), options);
  }

  now() { return Math.round(this.clock()); }

  initialize() {
    this.db.exec(`
      PRAGMA foreign_keys = ON;
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

      CREATE TABLE IF NOT EXISTS device_capture_state (
        device_id TEXT PRIMARY KEY,
        reachable INTEGER,
        is_sleepy INTEGER NOT NULL DEFAULT 0,
        max_report_interval_ms INTEGER NOT NULL DEFAULT 300000,
        updated_at INTEGER NOT NULL
      );
      CREATE TABLE IF NOT EXISTS capture_runs (
        id INTEGER PRIMARY KEY,
        started_at INTEGER NOT NULL,
        last_heartbeat_at INTEGER NOT NULL,
        ended_at INTEGER
      );
      CREATE TABLE IF NOT EXISTS capture_gaps (
        id INTEGER PRIMARY KEY,
        series_id INTEGER NOT NULL,
        started_at INTEGER NOT NULL,
        ended_at INTEGER,
        reason TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        FOREIGN KEY(series_id) REFERENCES series(id) ON DELETE CASCADE
      );
      CREATE INDEX IF NOT EXISTS idx_capture_gaps_series_time
        ON capture_gaps(series_id, started_at, ended_at);
      CREATE UNIQUE INDEX IF NOT EXISTS idx_capture_gaps_open_reason
        ON capture_gaps(series_id, reason) WHERE ended_at IS NULL;
    `);
    this.backfillLegacyGaps();
    this.removeNoReportGaps();
  }

  getSetting(key) {
    return this.db.prepare('SELECT value FROM settings WHERE key = ?').get(key)?.value;
  }

  setSetting(key, value) {
    this.db.prepare('INSERT INTO settings(key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value').run(key, String(value));
  }

  notify(change) {
    if (this.onChange) this.onChange(change);
  }

  backfillLegacyGaps() {
    if (this.getSetting('capture_gaps_backfill_v1')) return;
    const migratedAt = this.now();
    const seriesRows = this.db.prepare('SELECT id FROM series').all();
    const allReadings = this.db.prepare('SELECT captured_at, source FROM readings WHERE series_id = ? ORDER BY captured_at');
    const insertGap = this.db.prepare('INSERT INTO capture_gaps(series_id, started_at, ended_at, reason, created_at) VALUES (?, ?, ?, ?, ?)');

    for (const series of seriesRows) {
      const rows = allReadings.all(series.id);
      const events = rows.filter(row => row.source === 'event');
      const snapshots = rows.filter(row => row.source === 'state_snapshot');

      let legacySnapshotOpen = false;
      for (const snapshot of snapshots) {
        const previous = events.filter(row => row.captured_at <= snapshot.captured_at).at(-1);
        const next = events.find(row => row.captured_at > snapshot.captured_at);
        if (previous && next && previous.captured_at < next.captured_at) {
          insertGap.run(series.id, previous.captured_at, next.captured_at, 'legacy_snapshot', migratedAt);
        } else if (previous && !legacySnapshotOpen) {
          insertGap.run(series.id, previous.captured_at, null, 'legacy_snapshot', migratedAt);
          legacySnapshotOpen = true;
        }
      }

    }
    this.setSetting('capture_gaps_backfill_v1', migratedAt);
  }

  removeNoReportGaps() {
    if (this.getSetting('remove_no_report_gaps_v1')) return;
    this.db.prepare("DELETE FROM capture_gaps WHERE reason IN ('capture_stale', 'legacy_silence')").run();
    this.setSetting('remove_no_report_gaps_v1', this.now());
  }

  upsertDevice(device) {
    if (!device?.device_id) return;
    const timestamp = this.now();
    this.db.prepare(`INSERT INTO devices(device_id, name, manufacturer, model, capabilities_json, updated_at)
      VALUES (?, ?, ?, ?, ?, ?)
      ON CONFLICT(device_id) DO UPDATE SET name=excluded.name, manufacturer=excluded.manufacturer,
      model=excluded.model, capabilities_json=excluded.capabilities_json, updated_at=excluded.updated_at`)
      .run(device.device_id, device.name || device.device_id, device.manufacturer || null, device.model || null,
        JSON.stringify(Array.isArray(device.capabilities) ? device.capabilities : []), timestamp);
    this.updateDeviceCaptureState(device.device_id, { isSleepy: device.is_sleepy, reachable: device.online });
  }

  updateDeviceCaptureState(deviceId, { reachable, isSleepy, timestamp } = {}) {
    if (!deviceId) return;
    const current = this.db.prepare('SELECT * FROM device_capture_state WHERE device_id = ?').get(deviceId);
    const nextReachable = typeof reachable === 'boolean' ? Number(reachable) : current?.reachable ?? null;
    const nextSleepy = typeof isSleepy === 'boolean' ? Number(isSleepy) : current?.is_sleepy ?? 0;
    const updatedAt = Number.isFinite(timestamp) ? Math.round(timestamp) : this.now();
    this.db.prepare(`INSERT INTO device_capture_state(device_id, reachable, is_sleepy, updated_at)
      VALUES (?, ?, ?, ?)
      ON CONFLICT(device_id) DO UPDATE SET reachable=excluded.reachable, is_sleepy=excluded.is_sleepy,
      updated_at=excluded.updated_at`)
      .run(deviceId, nextReachable, nextSleepy, updatedAt);

    if (typeof reachable === 'boolean') this.setDeviceReachability(deviceId, reachable, updatedAt);
  }

  setDeviceReachability(deviceId, reachable, timestamp = this.now()) {
    const enabled = this.db.prepare('SELECT id FROM series WHERE device_id = ? AND enabled = 1').all(deviceId);
    for (const series of enabled) {
      if (reachable) this.closeGap(series.id, 'sensor_unreachable', timestamp);
      else this.openGap(series.id, 'sensor_unreachable', timestamp);
    }
  }

  cacheAttribute(deviceId, metric, attribute, timestamp = this.now()) {
    if (!deviceId || !metric || !attribute || !Object.hasOwn(attribute, 'value')) return null;
    const value = attribute.value;
    const unit = typeof attribute.unit === 'string' ? attribute.unit : null;
    const encoded = JSON.stringify(value);
    this.db.prepare(`INSERT INTO device_attributes(device_id, metric, unit, value_type, last_value_json, last_seen_at)
      VALUES (?, ?, ?, ?, ?, ?)
      ON CONFLICT(device_id, metric) DO UPDATE SET unit=excluded.unit, value_type=excluded.value_type,
      last_value_json=excluded.last_value_json, last_seen_at=excluded.last_seen_at`)
      .run(deviceId, metric, unit, typeof value, encoded, timestamp);
    return { value, unit, numericValue: numericValue(value), capturedAt: timestamp };
  }

  recordSnapshot(deviceId, metric, attribute) {
    return this.cacheAttribute(deviceId, metric, attribute);
  }

  recordReachableSnapshot(deviceId, metric, attribute, timestamp = this.now()) {
    const sample = this.cacheAttribute(deviceId, metric, attribute, timestamp);
    if (!sample) return null;
    const series = this.db.prepare('SELECT id FROM series WHERE device_id = ? AND metric = ? AND enabled = 1').get(deviceId, metric);
    if (!series) return null;
    this.db.prepare('INSERT INTO readings(series_id, captured_at, value_json, numeric_value, unit, source) VALUES (?, ?, ?, ?, ?, ?)')
      .run(series.id, sample.capturedAt, JSON.stringify(sample.value), sample.numericValue, sample.unit, 'reachable_snapshot');
    for (const reason of ['awaiting_first_sample', 'legacy_snapshot']) this.closeGap(series.id, reason, sample.capturedAt);
    const change = { type: 'reading', seriesId: series.id, ...sample, source: 'reachable_snapshot' };
    this.notify(change);
    return change;
  }

  recordLiveAttribute(deviceId, metric, attribute, timestamp = this.now()) {
    const sample = this.cacheAttribute(deviceId, metric, attribute, timestamp);
    if (!sample) return null;
    this.setDeviceReachability(deviceId, true, timestamp);
    const series = this.db.prepare('SELECT id FROM series WHERE device_id = ? AND metric = ? AND enabled = 1').get(deviceId, metric);
    if (!series) return null;
    this.db.prepare('INSERT INTO readings(series_id, captured_at, value_json, numeric_value, unit, source) VALUES (?, ?, ?, ?, ?, ?)')
      .run(series.id, sample.capturedAt, JSON.stringify(sample.value), sample.numericValue, sample.unit, 'event');
    for (const reason of ['awaiting_first_sample', 'legacy_snapshot']) this.closeGap(series.id, reason, sample.capturedAt);
    const change = { type: 'reading', seriesId: series.id, ...sample, source: 'event' };
    this.notify(change);
    return change;
  }

  openGap(seriesId, reason, startedAt = this.now()) {
    if (!GAP_REASONS.has(reason)) throw new Error(`Motivo de hueco no válido: ${reason}`);
    const existing = this.db.prepare('SELECT id FROM capture_gaps WHERE series_id = ? AND reason = ? AND ended_at IS NULL').get(seriesId, reason);
    if (existing) return existing.id;
    const result = this.db.prepare('INSERT INTO capture_gaps(series_id, started_at, ended_at, reason, created_at) VALUES (?, ?, NULL, ?, ?)')
      .run(seriesId, Math.round(startedAt), reason, this.now());
    this.notify({ type: 'gap', action: 'opened', seriesId, gapId: Number(result.lastInsertRowid), reason });
    return Number(result.lastInsertRowid);
  }

  closeGap(seriesId, reason, endedAt = this.now()) {
    const gap = this.db.prepare('SELECT id, started_at FROM capture_gaps WHERE series_id = ? AND reason = ? AND ended_at IS NULL').get(seriesId, reason);
    if (!gap) return false;
    const end = Math.max(gap.started_at, Math.round(endedAt));
    this.db.prepare('UPDATE capture_gaps SET ended_at = ? WHERE id = ?').run(end, gap.id);
    this.notify({ type: 'gap', action: 'closed', seriesId, gapId: gap.id, reason, endedAt: end });
    return true;
  }

  openGapForEnabled(reason, startedAt = this.now()) {
    for (const series of this.db.prepare('SELECT id FROM series WHERE enabled = 1').all()) this.openGap(series.id, reason, startedAt);
  }

  closeGapForEnabled(reason, endedAt = this.now()) {
    for (const series of this.db.prepare('SELECT id FROM series WHERE enabled = 1').all()) this.closeGap(series.id, reason, endedAt);
  }

  startRun() {
    const timestamp = this.now();
    const interrupted = this.db.prepare('SELECT id, last_heartbeat_at FROM capture_runs WHERE ended_at IS NULL ORDER BY id').all();
    for (const run of interrupted) this.db.prepare('UPDATE capture_runs SET ended_at = ? WHERE id = ?').run(run.last_heartbeat_at, run.id);
    const lastRun = this.db.prepare('SELECT MAX(ended_at) AS ended_at FROM capture_runs WHERE ended_at IS NOT NULL').get();
    if (lastRun?.ended_at && lastRun.ended_at < timestamp) {
      const existingSeries = this.db.prepare('SELECT id FROM series WHERE enabled = 1 AND created_at <= ?').all(lastRun.ended_at);
      for (const series of existingSeries) {
        this.db.prepare('INSERT INTO capture_gaps(series_id, started_at, ended_at, reason, created_at) VALUES (?, ?, ?, ?, ?)')
          .run(series.id, lastRun.ended_at, timestamp, 'collector_stopped', timestamp);
        this.notify({ type: 'gap', action: 'closed', seriesId: series.id, reason: 'collector_stopped', endedAt: timestamp });
      }
    }
    const result = this.db.prepare('INSERT INTO capture_runs(started_at, last_heartbeat_at) VALUES (?, ?)').run(timestamp, timestamp);
    this.runId = Number(result.lastInsertRowid);
    this.ensureAwaitingFirstSamples(timestamp);
    return this.runId;
  }

  heartbeatRun() {
    if (this.runId !== null) this.db.prepare('UPDATE capture_runs SET last_heartbeat_at = ? WHERE id = ? AND ended_at IS NULL').run(this.now(), this.runId);
  }

  stopRun(timestamp = this.now()) {
    if (this.runId === null) return;
    this.db.prepare('UPDATE capture_runs SET last_heartbeat_at = ?, ended_at = ? WHERE id = ? AND ended_at IS NULL').run(timestamp, timestamp, this.runId);
    this.runId = null;
  }

  ensureAwaitingFirstSamples(timestamp = this.now()) {
    const seriesRows = this.db.prepare(`SELECT s.id, s.created_at FROM series s
      WHERE s.enabled = 1 AND NOT EXISTS (SELECT 1 FROM readings r WHERE r.series_id = s.id AND r.source IN ${CONFIRMED_READING_SOURCES})`).all();
    for (const series of seriesRows) this.openGap(series.id, 'awaiting_first_sample', Math.min(series.created_at, timestamp));
  }

  markGatewayDisconnected(timestamp = this.now()) { this.openGapForEnabled('gateway_disconnected', timestamp); }
  markGatewayConnected(timestamp = this.now()) { this.closeGapForEnabled('gateway_disconnected', timestamp); }

  createOrEnableSeries(deviceId, metric, label, timestamp = this.now()) {
    this.db.prepare(`INSERT INTO series(device_id, metric, label, enabled, created_at) VALUES (?, ?, ?, 1, ?)
      ON CONFLICT(device_id, metric) DO UPDATE SET label=excluded.label, enabled=1`).run(String(deviceId), String(metric), label, timestamp);
    const series = this.db.prepare('SELECT * FROM series WHERE device_id = ? AND metric = ?').get(String(deviceId), String(metric));
    this.openGap(series.id, 'awaiting_first_sample', timestamp);
    this.closeGap(series.id, 'series_paused', timestamp);
    const captureState = this.db.prepare('SELECT reachable FROM device_capture_state WHERE device_id = ?').get(series.device_id);
    if (captureState?.reachable === 0) this.openGap(series.id, 'sensor_unreachable', timestamp);
    this.markGatewayDisconnectedIfNeeded(series.id, timestamp);
    return series;
  }

  updateSeries(id, { label, enabled }, timestamp = this.now()) {
    const series = this.db.prepare('SELECT * FROM series WHERE id = ?').get(id);
    if (!series) return false;
    if (label) this.db.prepare('UPDATE series SET label = ? WHERE id = ?').run(label, id);
    if (typeof enabled === 'boolean' && Boolean(series.enabled) !== enabled) {
      this.db.prepare('UPDATE series SET enabled = ? WHERE id = ?').run(enabled ? 1 : 0, id);
      if (enabled) {
        this.closeGap(id, 'series_paused', timestamp);
        this.openGap(id, 'awaiting_first_sample', timestamp);
        this.markGatewayDisconnectedIfNeeded(id, timestamp);
      } else {
        this.openGap(id, 'series_paused', timestamp);
      }
    }
    return true;
  }

  markGatewayDisconnectedIfNeeded(seriesId, timestamp) {
    const state = this.getSetting('gatewayConnected');
    if (state !== '1') this.openGap(seriesId, 'gateway_disconnected', timestamp);
  }

  setGatewayConnected(connected, timestamp = this.now()) {
    this.setSetting('gatewayConnected', connected ? '1' : '0');
    if (connected) this.markGatewayConnected(timestamp);
    else this.markGatewayDisconnected(timestamp);
  }

  deleteSeries(id) {
    this.db.prepare('DELETE FROM capture_gaps WHERE series_id = ?').run(id);
    this.db.prepare('DELETE FROM readings WHERE series_id = ?').run(id);
    return this.db.prepare('DELETE FROM series WHERE id = ?').run(id).changes > 0;
  }

  getStatus() {
    const devices = this.db.prepare(`SELECT d.device_id, d.name, d.manufacturer, d.model, d.capabilities_json, d.updated_at,
      (SELECT COUNT(*) FROM series s WHERE s.device_id=d.device_id AND s.enabled=1) AS tracked_count
      FROM devices d
      UNION
      SELECT a.device_id, a.device_id AS name, NULL AS manufacturer, NULL AS model, '[]' AS capabilities_json, a.last_seen_at AS updated_at,
      (SELECT COUNT(*) FROM series s WHERE s.device_id=a.device_id AND s.enabled=1) AS tracked_count
      FROM device_attributes a WHERE NOT EXISTS (SELECT 1 FROM devices d WHERE d.device_id=a.device_id)
      ORDER BY name COLLATE NOCASE`).all().map(row => ({ ...row, capabilities: JSON.parse(row.capabilities_json) }));
    const attributes = this.db.prepare('SELECT device_id, metric, unit, value_type, last_value_json, last_seen_at FROM device_attributes ORDER BY metric').all()
      .map(row => ({ ...row, last_value: JSON.parse(row.last_value_json) }));
    const series = this.db.prepare(`SELECT s.*, d.name AS device_name,
      COALESCE((SELECT r.unit FROM readings r WHERE r.series_id=s.id AND r.source IN ${CONFIRMED_READING_SOURCES} ORDER BY r.captured_at DESC LIMIT 1), a.unit) AS unit,
      (SELECT r.value_json FROM readings r WHERE r.series_id=s.id AND r.source IN ${CONFIRMED_READING_SOURCES} ORDER BY r.captured_at DESC LIMIT 1) AS last_value_json,
      (SELECT r.captured_at FROM readings r WHERE r.series_id=s.id AND r.source IN ${CONFIRMED_READING_SOURCES} ORDER BY r.captured_at DESC LIMIT 1) AS last_seen_at,
      (SELECT COUNT(*) FROM readings r WHERE r.series_id=s.id AND r.source IN ${CONFIRMED_READING_SOURCES}) AS reading_count
      FROM series s
      LEFT JOIN devices d ON d.device_id=s.device_id
      LEFT JOIN device_attributes a ON a.device_id=s.device_id AND a.metric=s.metric
      ORDER BY d.name COLLATE NOCASE, s.metric`).all().map(row => ({ ...row, last_value: row.last_value_json ? JSON.parse(row.last_value_json) : null }));
    return { devices, attributes, series };
  }

  getHistory(id, from, to) {
    const series = this.db.prepare('SELECT * FROM series WHERE id = ?').get(id);
    if (!series) return null;
    const readings = this.db.prepare(`SELECT captured_at, value_json, numeric_value, unit, source FROM readings
      WHERE series_id = ? AND source IN ${CONFIRMED_READING_SOURCES} AND captured_at >= ? AND captured_at < ? ORDER BY captured_at`).all(id, from, to).map(decodeReading);
    const previousReading = decodeReading(this.db.prepare(`SELECT captured_at, value_json, numeric_value, unit, source FROM readings
      WHERE series_id = ? AND source IN ${CONFIRMED_READING_SOURCES} AND captured_at < ? ORDER BY captured_at DESC LIMIT 1`).get(id, from));
    const nextReading = decodeReading(this.db.prepare(`SELECT captured_at, value_json, numeric_value, unit, source FROM readings
      WHERE series_id = ? AND source IN ${CONFIRMED_READING_SOURCES} AND captured_at >= ? ORDER BY captured_at ASC LIMIT 1`).get(id, to));
    // Include a gap that ended just before `from` when it separates the
    // contextual previous point from the first visible point. The UI clips its
    // band to the viewport, but still needs it to avoid inventing a segment.
    const gapContextFrom = previousReading?.captured_at ?? from;
    const gaps = this.db.prepare(`SELECT id, started_at, ended_at, reason FROM capture_gaps
      WHERE series_id = ? AND started_at < ? AND (ended_at IS NULL OR ended_at > ?)
      ORDER BY started_at, id`).all(id, to, gapContextFrom);
    const readingCount = this.db.prepare(`SELECT COUNT(*) AS count FROM readings WHERE series_id = ? AND source IN ${CONFIRMED_READING_SOURCES}`).get(id).count;
    return { series, from, to, readings, previousReading, nextReading, gaps, readingCount };
  }

  getHistoryDays(id, from, to, timeZone = 'UTC') {
    const series = this.db.prepare('SELECT id FROM series WHERE id = ?').get(id);
    if (!series) return null;
    const formatter = dayFormatter(timeZone);
    const rows = this.db.prepare(`SELECT captured_at FROM readings
      WHERE series_id = ? AND source IN ${CONFIRMED_READING_SOURCES} AND numeric_value IS NOT NULL
        AND captured_at >= ? AND captured_at < ? ORDER BY captured_at`).all(id, from, to);
    return { from, to, days: [...new Set(rows.map(row => formattedDay(formatter, row.captured_at)))].sort() };
  }
}
