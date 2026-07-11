import assert from 'node:assert/strict';
import test from 'node:test';
import { mergeGapIntervals, naturalTimeTicks, splitReadingsAtGaps } from '../public/chart-utils.js';

test('overlapping gaps are merged for their visual band', () => {
  const bands = mergeGapIntervals([
    { started_at: 20, ended_at: 50, reason: 'gateway_disconnected' },
    { started_at: 30, ended_at: 70, reason: 'sensor_unreachable' },
  ], 0, 100);
  assert.deepEqual(bands, [{ start: 20, end: 70, reasons: ['gateway_disconnected', 'sensor_unreachable'] }]);
});

test('a gap breaks the line between confirmed readings', () => {
  const readings = [
    { captured_at: 10, numeric_value: 20 },
    { captured_at: 20, numeric_value: 21 },
    { captured_at: 90, numeric_value: 22 },
  ];
  const segments = splitReadingsAtGaps(readings, [{ started_at: 30, ended_at: 80 }], 100);
  assert.deepEqual(segments.map(segment => segment.map(reading => reading.captured_at)), [[10, 20], [90]]);
});

test('time ticks stay aligned to natural clock boundaries while panning', () => {
  const from = new Date(2026, 6, 11, 12, 3).getTime();
  const to = new Date(2026, 6, 11, 14, 3).getTime();
  const ticks = naturalTimeTicks(from, to);
  assert.deepEqual(ticks.map(time => {
    const date = new Date(time);
    return [date.getHours(), date.getMinutes()];
  }), [[12, 30], [13, 0], [13, 30], [14, 0]]);
});
