import assert from 'node:assert/strict';
import test from 'node:test';
import { DatabaseSync } from 'node:sqlite';
import { HistoryStore } from '../history-store.js';

function fixture(start = 1_700_000_000_000) {
  let time = start;
  const db = new DatabaseSync(':memory:');
  const store = new HistoryStore(db, { clock: () => time });
  return { db, store, now: () => time, setTime: value => { time = value; } };
}

function history(store, id, from = 0, to = Number.MAX_SAFE_INTEGER) {
  return store.getHistory(id, from, to);
}

test('only snapshots from reachable sensors become confirmed initial readings', () => {
  const { store, now } = fixture();
  const series = store.createOrEnableSeries('sensor-1', 'temperature', 'Temperatura');
  store.recordSnapshot('sensor-1', 'temperature', { value: 22.3, unit: 'C' });
  assert.equal(history(store, series.id).readings.length, 0);
  assert.equal(store.getStatus().attributes[0].last_value, 22.3);

  store.recordReachableSnapshot('sensor-1', 'temperature', { value: 22.5, unit: 'C' }, now());
  const result = history(store, series.id);
  assert.equal(result.readings.length, 1);
  assert.equal(result.readings[0].value, 22.5);
  assert.equal(result.readings[0].source, 'reachable_snapshot');
  assert.ok(!result.gaps.some(gap => gap.reason === 'awaiting_first_sample' && gap.ended_at === null));
});

test('captures explicit bridge and sensor gaps and closes them only with evidence', () => {
  const { store, now, setTime } = fixture();
  const series = store.createOrEnableSeries('sensor-1', 'humidity', 'Humedad');
  store.setGatewayConnected(true, now());
  store.recordLiveAttribute('sensor-1', 'humidity', { value: 45, unit: '%' }, now());

  setTime(now() + 1_000);
  store.setGatewayConnected(false, now());
  let result = history(store, series.id);
  assert.ok(result.gaps.some(gap => gap.reason === 'gateway_disconnected' && gap.ended_at === null));

  setTime(now() + 1_000);
  store.setGatewayConnected(true, now());
  store.setDeviceReachability('sensor-1', false, now());
  result = history(store, series.id);
  assert.ok(result.gaps.some(gap => gap.reason === 'sensor_unreachable' && gap.ended_at === null));

  store.recordLiveAttribute('sensor-1', 'humidity', { value: 47, unit: '%' }, now());
  result = history(store, series.id);
  assert.equal(result.readings.length, 2);
  assert.ok(result.gaps.some(gap => gap.reason === 'sensor_unreachable' && gap.ended_at !== null));
});

test('does not infer a gap from quiet sensors and removes prior inferred gaps', () => {
  const { db, store, now, setTime } = fixture();
  const series = store.createOrEnableSeries('sensor-1', 'temperature', 'Temperatura');
  store.recordLiveAttribute('sensor-1', 'temperature', { value: 20, unit: 'C' }, now());
  setTime(now() + 24 * 3_600_000);
  assert.ok(!history(store, series.id).gaps.some(gap => gap.reason === 'capture_stale' || gap.reason === 'legacy_silence'));

  db.prepare("INSERT INTO capture_gaps(series_id, started_at, ended_at, reason, created_at) VALUES (?, ?, ?, 'capture_stale', ?)")
    .run(series.id, now() - 1_000, now(), now());
  db.prepare("DELETE FROM settings WHERE key = 'remove_no_report_gaps_v1'").run();
  new HistoryStore(db, { clock: now });
  assert.ok(!history(store, series.id).gaps.some(gap => gap.reason === 'capture_stale' || gap.reason === 'legacy_silence'));
});

test('records the period between collector runs, including a prior unclean exit', () => {
  const { db, store, now, setTime } = fixture();
  const series = store.createOrEnableSeries('sensor-1', 'temperature', 'Temperatura');
  store.startRun();
  store.recordLiveAttribute('sensor-1', 'temperature', { value: 20, unit: 'C' }, now());
  store.heartbeatRun();
  const stoppedAt = now();

  setTime(stoppedAt + 3_600_000);
  const restarted = new HistoryStore(db, { clock: now });
  restarted.startRun();
  const result = history(restarted, series.id);
  const outage = result.gaps.find(gap => gap.reason === 'collector_stopped');
  assert.equal(outage.started_at, stoppedAt);
  assert.equal(outage.ended_at, stoppedAt + 3_600_000);
});

test('pausing and resuming a series is persisted as a capture gap', () => {
  const { store, now, setTime } = fixture();
  const series = store.createOrEnableSeries('sensor-1', 'temperature', 'Temperatura');
  store.recordLiveAttribute('sensor-1', 'temperature', { value: 20, unit: 'C' }, now());
  setTime(now() + 10_000);
  store.updateSeries(series.id, { enabled: false }, now());
  setTime(now() + 20_000);
  store.updateSeries(series.id, { enabled: true }, now());
  const pause = history(store, series.id).gaps.find(gap => gap.reason === 'series_paused');
  assert.equal(pause.started_at, now() - 20_000);
  assert.equal(pause.ended_at, now());
});

test('legacy migration preserves rows, omits snapshots from history and is idempotent', () => {
  const { db, store, now } = fixture();
  const series = store.createOrEnableSeries('sensor-1', 'temperature', 'Temperatura');
  db.prepare("INSERT INTO readings(series_id, captured_at, value_json, numeric_value, unit, source) VALUES (?, ?, ?, ?, ?, ?)").run(series.id, now() - 8_000_000, '20', 20, 'C', 'event');
  db.prepare("INSERT INTO readings(series_id, captured_at, value_json, numeric_value, unit, source) VALUES (?, ?, ?, ?, ?, ?)").run(series.id, now() - 4_000_000, '20', 20, 'C', 'state_snapshot');
  db.prepare("INSERT INTO readings(series_id, captured_at, value_json, numeric_value, unit, source) VALUES (?, ?, ?, ?, ?, ?)").run(series.id, now() - 1_000_000, '21', 21, 'C', 'event');
  db.prepare("DELETE FROM settings WHERE key = 'capture_gaps_backfill_v1'").run();
  store.backfillLegacyGaps();
  const gapCount = db.prepare('SELECT COUNT(*) AS count FROM capture_gaps').get().count;
  store.backfillLegacyGaps();
  assert.equal(db.prepare('SELECT COUNT(*) AS count FROM capture_gaps').get().count, gapCount);
  assert.equal(history(store, series.id).readings.length, 2);
  assert.ok(history(store, series.id).gaps.some(gap => gap.reason === 'legacy_snapshot'));
});

test('history includes a preceding gap needed to avoid joining the contextual point', () => {
  const { store, now } = fixture();
  const series = store.createOrEnableSeries('sensor-1', 'temperature', 'Temperatura');
  store.recordLiveAttribute('sensor-1', 'temperature', { value: 20, unit: 'C' }, now());
  store.openGap(series.id, 'gateway_disconnected', now() + 1_000);
  store.closeGap(series.id, 'gateway_disconnected', now() + 2_000);
  store.recordLiveAttribute('sensor-1', 'temperature', { value: 21, unit: 'C' }, now() + 4_000);
  const result = history(store, series.id, now() + 3_000, now() + 5_000);
  assert.equal(result.previousReading.captured_at, now());
  assert.ok(result.gaps.some(gap => gap.reason === 'gateway_disconnected'));
});

test('history days returns only local days with numeric confirmed readings', () => {
  const { store, now } = fixture();
  const series = store.createOrEnableSeries('sensor-1', 'temperature', 'Temperatura');
  store.recordLiveAttribute('sensor-1', 'temperature', { value: 20, unit: 'C' }, now());
  store.recordLiveAttribute('sensor-1', 'temperature', { value: 21, unit: 'C' }, now() + 24 * 3_600_000);
  const result = store.getHistoryDays(series.id, now() - 1, now() + 48 * 3_600_000, 'UTC');
  assert.deepEqual(result.days, ['2023-11-14', '2023-11-15']);
});

test('history days uses the browser timezone for the calendar date', () => {
  const { store } = fixture(Date.UTC(2023, 6, 10, 22, 30));
  const series = store.createOrEnableSeries('sensor-1', 'temperature', 'Temperatura');
  store.recordLiveAttribute('sensor-1', 'temperature', { value: 20, unit: 'C' }, Date.UTC(2023, 6, 10, 22, 30));
  const result = store.getHistoryDays(series.id, Date.UTC(2023, 6, 10), Date.UTC(2023, 6, 12), 'Europe/Madrid');
  assert.deepEqual(result.days, ['2023-07-11']);
});
