export function mergeGapIntervals(gaps, from, to) {
  const clipped = gaps
    .map(gap => ({
      start: Math.max(from, gap.started_at),
      end: Math.min(to, gap.ended_at ?? to),
      reasons: [gap.reason],
    }))
    .filter(gap => gap.start < gap.end)
    .sort((left, right) => left.start - right.start || left.end - right.end);

  return clipped.reduce((merged, gap) => {
    const previous = merged.at(-1);
    if (previous && gap.start <= previous.end) {
      previous.end = Math.max(previous.end, gap.end);
      previous.reasons = [...new Set([...previous.reasons, ...gap.reasons])];
    } else {
      merged.push(gap);
    }
    return merged;
  }, []);
}

export function splitReadingsAtGaps(readings, gaps, rangeEnd = Number.POSITIVE_INFINITY) {
  const values = readings.filter(reading => Number.isFinite(reading.numeric_value)).sort((left, right) => left.captured_at - right.captured_at);
  if (!values.length) return [];
  const intersectsGap = (start, end) => gaps.some(gap => {
    const gapEnd = gap.ended_at ?? rangeEnd;
    return gap.started_at < end && gapEnd > start;
  });
  const segments = [[values[0]]];
  for (const reading of values.slice(1)) {
    const segment = segments.at(-1);
    const previous = segment.at(-1);
    if (intersectsGap(previous.captured_at, reading.captured_at)) segments.push([reading]);
    else segment.push(reading);
  }
  return segments;
}

const TIME_TICK_INTERVALS = [
  { unit: 'minute', amount: 1 },
  { unit: 'minute', amount: 2 },
  { unit: 'minute', amount: 5 },
  { unit: 'minute', amount: 10 },
  { unit: 'minute', amount: 15 },
  { unit: 'minute', amount: 30 },
  { unit: 'hour', amount: 1 },
  { unit: 'hour', amount: 2 },
  { unit: 'hour', amount: 3 },
  { unit: 'hour', amount: 4 },
  { unit: 'hour', amount: 6 },
  { unit: 'hour', amount: 12 },
  { unit: 'day', amount: 1 },
  { unit: 'day', amount: 2 },
  { unit: 'day', amount: 7 },
];

function intervalDuration(interval) {
  if (interval.unit === 'minute') return interval.amount * 60_000;
  if (interval.unit === 'hour') return interval.amount * 3_600_000;
  return interval.amount * 86_400_000;
}

function firstLocalBoundaryAfter(from, interval) {
  const boundary = new Date(from);
  boundary.setSeconds(0, 0);

  if (interval.unit === 'minute') {
    boundary.setMinutes(Math.floor(boundary.getMinutes() / interval.amount) * interval.amount);
    while (boundary.getTime() < from) boundary.setMinutes(boundary.getMinutes() + interval.amount);
    return boundary;
  }

  if (interval.unit === 'hour') {
    boundary.setMinutes(0, 0, 0);
    boundary.setHours(Math.floor(boundary.getHours() / interval.amount) * interval.amount);
    while (boundary.getTime() < from) boundary.setHours(boundary.getHours() + interval.amount);
    return boundary;
  }

  boundary.setHours(0, 0, 0, 0);
  while (boundary.getTime() < from) boundary.setDate(boundary.getDate() + interval.amount);
  return boundary;
}

function advanceLocalBoundary(boundary, interval) {
  if (interval.unit === 'minute') boundary.setMinutes(boundary.getMinutes() + interval.amount);
  else if (interval.unit === 'hour') boundary.setHours(boundary.getHours() + interval.amount);
  else boundary.setDate(boundary.getDate() + interval.amount);
}

/**
 * Returns local-time grid marks at natural clock boundaries. The marks are
 * deliberately independent of the viewport start/end, so panning from 12:03
 * keeps labels such as 12:30 and 13:00 instead of inventing 12:31 or 12:58.
 */
export function naturalTimeTicks(from, to, targetDivisions = 4) {
  if (!Number.isFinite(from) || !Number.isFinite(to) || from > to) return [];
  const target = Math.max(1, (to - from) / Math.max(1, targetDivisions));
  const interval = TIME_TICK_INTERVALS.reduce((best, candidate) => (
    Math.abs(intervalDuration(candidate) - target) < Math.abs(intervalDuration(best) - target) ? candidate : best
  ));
  const ticks = [];
  const boundary = firstLocalBoundaryAfter(from, interval);
  while (boundary.getTime() <= to) {
    ticks.push(boundary.getTime());
    advanceLocalBoundary(boundary, interval);
  }
  return ticks;
}
