import { mergeGapIntervals, naturalTimeTicks, splitReadingsAtGaps } from './chart-utils.js';

let status = null;
const $ = selector => document.querySelector(selector);
const escapeHtml = value => String(value ?? '').replace(/[&<>'"]/g, char => ({ '&':'&amp;', '<':'&lt;', '>':'&gt;', "'":'&#39;', '"':'&quot;' }[char]));
const formatValue = value => typeof value === 'boolean' ? (value ? 'Sí' : 'No') : typeof value === 'object' ? JSON.stringify(value) : String(value ?? '—');
const formatDate = time => time ? new Intl.DateTimeFormat('es-ES', { dateStyle:'short', timeStyle:'short' }).format(time) : 'sin captura confirmada';
const GAP_LABELS = {
  collector_stopped: 'Aplicación de histórico detenida',
  gateway_disconnected: 'Puente Zigbee sin conexión',
  sensor_unreachable: 'Sensor no accesible',
  awaiting_first_sample: 'En espera de la primera lectura confirmada',
  series_paused: 'Serie pausada',
  legacy_snapshot: 'Estado anterior del puente, no confirmado',
};

async function request(url, options) {
  const response = await fetch(url, options);
  const data = response.status === 204 ? null : await response.json();
  if (!response.ok) throw new Error(data?.error || 'No se pudo completar la operación.');
  return data;
}

function attributesFor(deviceId) { return (status?.attributes || []).filter(attribute => attribute.device_id === deviceId); }
function gapDescription(reasons) { return reasons.map(reason => GAP_LABELS[reason] || reason).join(' · '); }

function renderConnection() {
  const connection = $('#connection'); const gateway = status.gateway;
  connection.className = `connection ${gateway.connected ? 'ok' : gateway.error ? 'error' : ''}`;
  connection.textContent = gateway.connected ? '● Puente conectado' : gateway.error ? `● ${gateway.error}` : '● Conectando al puente…';
  $('#gateway-url').value = status.settings.gatewayUrl;
  $('#last-message').textContent = gateway.lastMessageAt ? `Último mensaje: ${formatDate(gateway.lastMessageAt)}` : 'Aún sin mensajes';
}

function renderDeviceOptions() {
  const select = $('#device-select'); const selected = select.value;
  select.innerHTML = '<option value="">Selecciona un dispositivo</option>' + status.devices.map(device => `<option value="${escapeHtml(device.device_id)}">${escapeHtml(device.name)} · ${escapeHtml(device.device_id)}</option>`).join('');
  if ([...select.options].some(option => option.value === selected)) select.value = selected;
  $('#device-count').textContent = `${status.devices.length} dispositivo${status.devices.length === 1 ? '' : 's'} descubierto${status.devices.length === 1 ? '' : 's'}`;
  renderMetricOptions();
}

function renderMetricOptions() {
  const device = $('#device-select').value; const select = $('#metric-select'); const current = select.value; const attributes = attributesFor(device);
  select.innerHTML = '<option value="">Selecciona un atributo</option>' + attributes.map(attribute => `<option value="${escapeHtml(attribute.metric)}">${escapeHtml(attribute.metric)}${attribute.unit ? ` (${escapeHtml(attribute.unit)})` : ''} · ${escapeHtml(formatValue(attribute.last_value))}</option>`).join('');
  if ([...select.options].some(option => option.value === current)) select.value = current;
}

function gapRects(gaps, from, to, x, top, height, className, interactive = false) {
  return mergeGapIntervals(gaps, from, to).map(gap => {
    const width = Math.max(1, x(gap.end) - x(gap.start));
    const data = interactive ? ` data-gap-start="${gap.start}" data-gap-end="${gap.end}" data-gap-reasons="${escapeHtml(gap.reasons.join(','))}"` : '';
    return `<rect class="${className}" x="${x(gap.start).toFixed(2)}" y="${top}" width="${width.toFixed(2)}" height="${height}"${data}/>`;
  }).join('');
}

function svgChart(readings, gaps, from, to) {
  const values = readings.filter(item => Number.isFinite(item.numeric_value));
  const x = value => 36 + ((value - from) / (to - from)) * 752;
  const bands = gapRects(gaps, from, to, x, 28, 190, 'gap-band');
  const grid = '<defs><pattern id="history-gap-hatch" width="8" height="8" patternUnits="userSpaceOnUse" patternTransform="rotate(45)"><line x1="0" y1="0" x2="0" y2="8"/></pattern></defs><line class="grid" x1="36" y1="28" x2="788" y2="28"/><line class="grid" x1="36" y1="123" x2="788" y2="123"/><line class="grid" x1="36" y1="218" x2="788" y2="218"/>';
  if (!values.length) return `${grid}${bands}<text x="400" y="125" text-anchor="middle" fill="#6b7783" font-size="16">Aún no hay valores numéricos confirmados</text>`;
  const rawMin = Math.min(...values.map(item => item.numeric_value));
  const rawMax = Math.max(...values.map(item => item.numeric_value));
  const margin = rawMin === rawMax ? Math.max(Math.abs(rawMin) * .05, 1) : (rawMax - rawMin) * .08;
  const minY = rawMin - margin; const maxY = rawMax + margin;
  const y = value => 218 - ((value - minY) / (maxY - minY)) * 190;
  const lines = splitReadingsAtGaps(values, gaps, to).filter(segment => segment.length > 1)
    .map(segment => `<polyline class="line" points="${segment.map(item => `${x(item.captured_at).toFixed(1)},${y(item.numeric_value).toFixed(1)}`).join(' ')}"/>`).join('');
  const dots = values.length < 70 ? values.map(item => `<circle class="dot" cx="${x(item.captured_at).toFixed(1)}" cy="${y(item.numeric_value).toFixed(1)}" r="3"/>`).join('') : '';
  return `${grid}${bands}<text x="2" y="33" fill="#6b7783" font-size="12">${rawMax.toFixed(2)}</text><text x="2" y="222" fill="#6b7783" font-size="12">${rawMin.toFixed(2)}</text>${lines}${dots}`;
}

const explorer = { active: null, readings: new Map(), gaps: new Map(), ranges: [], view: null, drag: null, selection: null, pendingPan: null, panning: false, operation: 0, calendarMonth: null, calendarDays: new Set(), calendarOperation: 0 };
const explorerPlot = { left: 78, top: 28, width: 892, height: 500 };
const explorerDialog = () => $('#history-explorer');
const explorerChart = () => $('#history-explorer-chart');
const explorerDate = () => $('#history-explorer-date');
const explorerCalendar = () => $('#history-explorer-calendar');
const explorerNotice = () => $('#history-explorer-notice');
const explorerTooltip = () => $('#history-explorer-tooltip');

function localDateValue(date) { return `${date.getFullYear()}-${String(date.getMonth() + 1).padStart(2, '0')}-${String(date.getDate()).padStart(2, '0')}`; }
function localDateFromValue(value) { const [year, month, day] = value.split('-').map(Number); return year && month && day ? new Date(year, month - 1, day) : null; }
function localDayBounds(date = new Date()) {
  const from = new Date(date.getFullYear(), date.getMonth(), date.getDate());
  const to = new Date(date.getFullYear(), date.getMonth(), date.getDate() + 1);
  return { from: from.getTime(), to: to.getTime() };
}
function localMonthBounds(date) {
  const from = new Date(date.getFullYear(), date.getMonth(), 1);
  const to = new Date(date.getFullYear(), date.getMonth() + 1, 1);
  return { from: from.getTime(), to: to.getTime() };
}
function explorerNumericReadings() { return [...explorer.readings.values()].filter(reading => Number.isFinite(reading.numeric_value)).sort((a, b) => a.captured_at - b.captured_at); }
function explorerReadingsIn(view) { return explorerNumericReadings().filter(reading => reading.captured_at >= view.from && reading.captured_at <= view.to && reading.numeric_value >= view.yMin && reading.numeric_value <= view.yMax); }
function explorerGapsIn(view) { return [...explorer.gaps.values()].filter(gap => gap.started_at < view.to && (gap.ended_at === null || gap.ended_at > view.from)); }
function explorerYRange(readings) {
  const values = readings.filter(reading => Number.isFinite(reading.numeric_value)).map(reading => reading.numeric_value);
  const minimum = Math.min(0, ...(values.length ? values : [0])); const maximum = Math.max(0, ...(values.length ? values : [0]));
  const span = maximum - minimum; const margin = span ? span * .08 : Math.max(Math.abs(maximum) * .08, 1);
  return { yMin: minimum - margin, yMax: maximum + margin };
}
function explorerTimeLabel(time, span) { return new Intl.DateTimeFormat('es-ES', span <= 36 * 3_600_000 ? { hour: '2-digit', minute: '2-digit' } : { day: '2-digit', month: 'short' }).format(time); }
function explorerExactDate(time) { return new Intl.DateTimeFormat('es-ES', { day: '2-digit', month: '2-digit', year: 'numeric', hour: '2-digit', minute: '2-digit', second: '2-digit' }).format(time); }
function setExplorerNotice(message = '', error = false) { const node = explorerNotice(); node.textContent = message; node.classList.toggle('error', error); }
function hideExplorerTooltip() { explorerTooltip().hidden = true; }
function showExplorerTooltip(target, event) {
  const tooltip = explorerTooltip(); const wrap = target.closest('.explorer-chart-wrap');
  if (target.classList.contains('explorer-gap')) {
    const reasons = target.dataset.gapReasons.split(',');
    tooltip.textContent = `Sin valores: ${gapDescription(reasons)} · ${explorerExactDate(Number(target.dataset.gapStart))} — ${explorerExactDate(Number(target.dataset.gapEnd))}`;
  } else {
    const value = target.dataset.readingValue; const unit = target.dataset.readingUnit;
    tooltip.textContent = `${value}${unit ? ` ${unit}` : ''} · ${explorerExactDate(Number(target.dataset.readingTime))}`;
  }
  tooltip.hidden = false;
  const bounds = wrap.getBoundingClientRect();
  tooltip.style.left = `${Math.min(wrap.clientWidth - tooltip.offsetWidth - 10, Math.max(10, event.clientX - bounds.left + 14))}px`;
  tooltip.style.top = `${Math.min(wrap.clientHeight - tooltip.offsetHeight - 10, Math.max(10, event.clientY - bounds.top + 14))}px`;
}
function mergeExplorerReadings(readings) { for (const reading of readings) explorer.readings.set(`${reading.captured_at}:${reading.numeric_value}`, reading); }
function mergeExplorerGaps(gaps) { for (const gap of gaps) explorer.gaps.set(gap.id, gap); }
function markExplorerRange(from, to) {
  explorer.ranges.push({ from, to }); explorer.ranges.sort((a, b) => a.from - b.from);
  explorer.ranges = explorer.ranges.reduce((merged, range) => {
    const previous = merged.at(-1); if (previous && range.from <= previous.to) previous.to = Math.max(previous.to, range.to); else merged.push({ ...range }); return merged;
  }, []);
}
function explorerRangeCached(from, to) { return explorer.ranges.some(range => range.from <= from && range.to >= to); }
async function loadExplorerRange(from, to) {
  if (explorerRangeCached(from, to)) return;
  const history = await request(`/api/history?seriesId=${explorer.active.id}&from=${Math.round(from)}&to=${Math.round(to)}`);
  mergeExplorerReadings([...history.readings, history.previousReading, history.nextReading].filter(Boolean));
  mergeExplorerGaps(history.gaps); markExplorerRange(from, to);
}
function syncExplorerDate() {
  const date = new Date((explorer.view.from + explorer.view.to) / 2);
  explorerDate().dataset.value = localDateValue(date);
  explorerDate().textContent = new Intl.DateTimeFormat('es-ES', { day:'2-digit', month:'short', year:'numeric' }).format(date);
}
function explorerTimeZone() { return Intl.DateTimeFormat().resolvedOptions().timeZone || 'UTC'; }
function calendarMonthTitle(date) { return new Intl.DateTimeFormat('es-ES', { month:'long', year:'numeric' }).format(date); }
async function loadExplorerCalendarMonth(date) {
  if (!explorer.active) return;
  const bounds = localMonthBounds(date); const operation = ++explorer.calendarOperation;
  const days = await request(`/api/history/days?seriesId=${explorer.active.id}&from=${bounds.from}&to=${bounds.to}&timeZone=${encodeURIComponent(explorerTimeZone())}`);
  if (operation !== explorer.calendarOperation || !explorer.active) return;
  explorer.calendarMonth = new Date(date.getFullYear(), date.getMonth(), 1);
  explorer.calendarDays = new Set(days.days);
  renderExplorerCalendar();
}
function renderExplorerCalendar() {
  const calendar = explorerCalendar(); const month = explorer.calendarMonth;
  if (!month) return;
  const selected = explorerDate().dataset.value;
  const start = (month.getDay() + 6) % 7; const count = new Date(month.getFullYear(), month.getMonth() + 1, 0).getDate();
  const blanks = Array.from({ length:start }, () => '<span></span>').join('');
  const days = Array.from({ length:count }, (_, index) => {
    const date = new Date(month.getFullYear(), month.getMonth(), index + 1); const value = localDateValue(date); const hasData = explorer.calendarDays.has(value);
    return `<button class="calendar-day${value === selected ? ' is-selected' : ''}" type="button" data-calendar-date="${value}"${hasData ? ' title="Tiene datos"' : ' disabled title="Sin datos"'}>${index + 1}${hasData ? '<span class="calendar-day-dot"></span>' : ''}</button>`;
  }).join('');
  calendar.innerHTML = `<div class="calendar-header"><button class="secondary" type="button" data-calendar-month="-1" aria-label="Mes anterior">‹</button><span>${escapeHtml(calendarMonthTitle(month))}</span><button class="secondary" type="button" data-calendar-month="1" aria-label="Mes siguiente">›</button></div><div class="calendar-weekdays"><span>L</span><span>M</span><span>X</span><span>J</span><span>V</span><span>S</span><span>D</span></div><div class="calendar-grid">${blanks}${days}</div>`;
}
async function toggleExplorerCalendar() {
  const calendar = explorerCalendar(); const opening = calendar.hidden;
  calendar.hidden = !opening; explorerDate().setAttribute('aria-expanded', String(opening));
  if (!opening) return;
  const selected = localDateFromValue(explorerDate().dataset.value) || new Date();
  try { await loadExplorerCalendarMonth(selected); } catch (error) { setExplorerNotice(error.message, true); }
}
function clampExplorerSvgPoint(event) {
  const rect = explorerChart().getBoundingClientRect();
  return { x: Math.min(explorerPlot.left + explorerPlot.width, Math.max(explorerPlot.left, (event.clientX - rect.left) * 1000 / rect.width)), y: Math.min(explorerPlot.top + explorerPlot.height, Math.max(explorerPlot.top, (event.clientY - rect.top) * 600 / rect.height)) };
}
function explorerCoordinates(point, view = explorer.view) {
  return { time: view.from + ((point.x - explorerPlot.left) / explorerPlot.width) * (view.to - view.from), value: view.yMax - ((point.y - explorerPlot.top) / explorerPlot.height) * (view.yMax - view.yMin) };
}

function renderExplorer() {
  if (!explorer.view) return;
  hideExplorerTooltip();
  const chart = explorerChart(); const view = explorer.view; const readings = explorerReadingsIn(view); const allReadings = explorerNumericReadings();
  const inTimeRange = allReadings.filter(reading => reading.captured_at >= view.from && reading.captured_at <= view.to);
  const previous = allReadings.filter(reading => reading.captured_at < view.from).at(-1); const next = allReadings.find(reading => reading.captured_at > view.to);
  const lineReadings = inTimeRange.length ? [previous, ...inTimeRange, next].filter(Boolean) : [];
  const gaps = explorerGapsIn(view);
  const lineGaps = [...explorer.gaps.values()];
  const x = time => explorerPlot.left + ((time - view.from) / (view.to - view.from)) * explorerPlot.width;
  const y = value => explorerPlot.top + ((view.yMax - value) / (view.yMax - view.yMin)) * explorerPlot.height;
  const verticalGrid = naturalTimeTicks(view.from, view.to).map(time => {
    const xPos = x(time);
    return `<line class="explorer-grid" x1="${xPos}" y1="${explorerPlot.top}" x2="${xPos}" y2="${explorerPlot.top + explorerPlot.height}"/><text class="explorer-axis" x="${xPos}" y="568" text-anchor="middle">${escapeHtml(explorerTimeLabel(time, view.to - view.from))}</text>`;
  }).join('');
  const horizontalGrid = Array.from({ length: 5 }, (_, index) => {
    const fraction = index / 4; const yPos = explorerPlot.top + fraction * explorerPlot.height; const value = view.yMax - fraction * (view.yMax - view.yMin);
    return `<line class="explorer-grid" x1="${explorerPlot.left}" y1="${yPos}" x2="${explorerPlot.left + explorerPlot.width}" y2="${yPos}"/><text class="explorer-axis" x="68" y="${yPos + 4}" text-anchor="end">${value.toFixed(2)}</text>`;
  }).join('');
  const lines = splitReadingsAtGaps(lineReadings, lineGaps, view.to).filter(segment => segment.length > 1).map(segment => `<polyline class="explorer-line" points="${segment.map(reading => `${x(reading.captured_at).toFixed(2)},${y(reading.numeric_value).toFixed(2)}`).join(' ')}"/>`).join('');
  const bands = gapRects(gaps, view.from, view.to, x, explorerPlot.top, explorerPlot.height, 'explorer-gap', true);
  const dots = readings.length < 160 ? readings.map(reading => `<circle class="explorer-dot" cx="${x(reading.captured_at).toFixed(2)}" cy="${y(reading.numeric_value).toFixed(2)}" r="3.5" data-reading-time="${reading.captured_at}" data-reading-value="${escapeHtml(String(reading.value ?? reading.numeric_value))}" data-reading-unit="${escapeHtml(reading.unit || explorer.active?.unit || '')}"/>`).join('') : '';
  const selection = explorer.selection ? `<rect class="explorer-selection" x="${Math.min(explorer.selection.start.x, explorer.selection.current.x)}" y="${Math.min(explorer.selection.start.y, explorer.selection.current.y)}" width="${Math.abs(explorer.selection.current.x - explorer.selection.start.x)}" height="${Math.abs(explorer.selection.current.y - explorer.selection.start.y)}"/>` : '';
  const empty = readings.length ? '' : '<text class="explorer-empty" x="524" y="280" text-anchor="middle">No hay valores numéricos confirmados en esta vista</text>';
  chart.innerHTML = `<defs><clipPath id="history-explorer-clip"><rect x="${explorerPlot.left}" y="${explorerPlot.top}" width="${explorerPlot.width}" height="${explorerPlot.height}"/></clipPath><pattern id="explorer-gap-hatch" width="10" height="10" patternUnits="userSpaceOnUse" patternTransform="rotate(45)"><line x1="0" y1="0" x2="0" y2="10"/></pattern></defs>${verticalGrid}${horizontalGrid}<rect class="explorer-frame" x="${explorerPlot.left}" y="${explorerPlot.top}" width="${explorerPlot.width}" height="${explorerPlot.height}"/><g clip-path="url(#history-explorer-clip)">${bands}${lines}${dots}${selection}</g>${empty}<text class="explorer-axis-title" x="${explorerPlot.left}" y="16">${escapeHtml(explorer.active?.unit || '')}</text>`;
  syncExplorerDate();
}

async function setExplorerView(candidate, { allowEmpty = false, showFailure = false } = {}) {
  const operation = ++explorer.operation;
  try {
    await loadExplorerRange(candidate.from, candidate.to);
    if (operation !== explorer.operation) return false;
    const visible = explorerReadingsIn(candidate);
    if (!allowEmpty && !visible.length) { if (showFailure) setExplorerNotice('El área elegida no contiene datos visibles.', true); return false; }
    explorer.view = candidate; renderExplorer(); return true;
  } catch (error) { if (operation === explorer.operation) setExplorerNotice(error.message, true); return false; }
}
async function resetExplorer() {
  if (!explorer.active) return;
  const day = localDayBounds(); setExplorerNotice('');
  try {
    await loadExplorerRange(day.from, day.to);
    explorer.view = { ...day, ...explorerYRange(explorerNumericReadings().filter(reading => reading.captured_at >= day.from && reading.captured_at < day.to)) };
    explorer.operation += 1; renderExplorer();
  } catch (error) { setExplorerNotice(error.message, true); }
}
async function openExplorer(item) {
  explorer.active = item; explorer.readings.clear(); explorer.gaps.clear(); explorer.ranges = []; explorer.view = null; explorer.selection = null; explorer.calendarMonth = null; explorer.calendarDays.clear(); explorer.operation += 1;
  $('#history-explorer-title').textContent = item.label; $('#history-explorer-subtitle').textContent = `${item.device_name || item.device_id} · ${item.metric}`;
  explorerDialog().showModal(); await resetExplorer();
}
async function selectExplorerDate(value) {
  const date = localDateFromValue(value); if (!date) return;
  const bounds = localDayBounds(date); setExplorerNotice(''); await loadExplorerRange(bounds.from, bounds.to);
  const readings = explorerNumericReadings().filter(reading => reading.captured_at >= bounds.from && reading.captured_at < bounds.to);
  if (!readings.length) { setExplorerNotice('No hay valores numéricos confirmados para ese día.', true); syncExplorerDate(); return; }
  explorer.operation += 1; explorer.view = { ...bounds, ...explorerYRange(readings) }; renderExplorer(); explorerCalendar().hidden = true; explorerDate().setAttribute('aria-expanded', 'false');
}
async function processExplorerPan() { if (explorer.panning) return; explorer.panning = true; while (explorer.pendingPan) { const candidate = explorer.pendingPan; explorer.pendingPan = null; await setExplorerView(candidate); } explorer.panning = false; }
function queueExplorerPan(candidate) { explorer.pendingPan = candidate; processExplorerPan(); }
function setupExplorer() {
  const dialog = explorerDialog(); const chart = explorerChart();
  $('#history-explorer-close').onclick = () => dialog.close(); $('#history-explorer-reset').onclick = resetExplorer;
  $('#history-explorer-zoom-out').onclick = async () => { const view = explorer.view; const xCenter = (view.from + view.to) / 2; const yCenter = (view.yMin + view.yMax) / 2; await setExplorerView({ from: xCenter - (view.to - view.from), to: xCenter + (view.to - view.from), yMin: yCenter - (view.yMax - view.yMin), yMax: yCenter + (view.yMax - view.yMin) }, { showFailure: true }); };
  explorerDate().onclick = toggleExplorerCalendar;
  explorerCalendar().onclick = event => {
    const day = event.target.closest('[data-calendar-date]'); const month = event.target.closest('[data-calendar-month]');
    if (day) selectExplorerDate(day.dataset.calendarDate);
    if (month && explorer.calendarMonth) loadExplorerCalendarMonth(new Date(explorer.calendarMonth.getFullYear(), explorer.calendarMonth.getMonth() + Number(month.dataset.calendarMonth), 1)).catch(error => setExplorerNotice(error.message, true));
  };
  dialog.addEventListener('close', () => { explorer.active = null; explorer.drag = null; explorer.selection = null; explorerCalendar().hidden = true; explorerDate().setAttribute('aria-expanded', 'false'); });
  chart.addEventListener('contextmenu', event => event.preventDefault());
  chart.addEventListener('pointerover', event => { if (event.target.matches('.explorer-dot, .explorer-gap')) showExplorerTooltip(event.target, event); });
  chart.addEventListener('pointermove', event => { if (event.target.matches('.explorer-dot, .explorer-gap')) showExplorerTooltip(event.target, event); });
  chart.addEventListener('pointerout', event => { if (event.target.matches('.explorer-dot, .explorer-gap')) hideExplorerTooltip(); });
  chart.addEventListener('pointerdown', event => {
    if (!explorer.view || (event.button !== 0 && event.button !== 2)) return;
    event.preventDefault(); chart.setPointerCapture(event.pointerId); const point = clampExplorerSvgPoint(event);
    explorer.drag = { pointerId: event.pointerId, button: event.button, startClientX: event.clientX, startClientY: event.clientY, startView: { ...explorer.view }, startPoint: point, rect: chart.getBoundingClientRect() };
    if (event.button === 2) { explorer.selection = { start: point, current: point }; renderExplorer(); }
  });
  chart.addEventListener('pointermove', event => {
    const drag = explorer.drag; if (!drag || drag.pointerId !== event.pointerId) return;
    if (drag.button === 2) { explorer.selection.current = clampExplorerSvgPoint(event); renderExplorer(); return; }
    const plotWidth = drag.rect.width * explorerPlot.width / 1000; const plotHeight = drag.rect.height * explorerPlot.height / 600;
    const xDelta = (event.clientX - drag.startClientX) * (drag.startView.to - drag.startView.from) / plotWidth; const yDelta = (event.clientY - drag.startClientY) * (drag.startView.yMax - drag.startView.yMin) / plotHeight;
    queueExplorerPan({ from: drag.startView.from - xDelta, to: drag.startView.to - xDelta, yMin: drag.startView.yMin + yDelta, yMax: drag.startView.yMax + yDelta });
  });
  const endDrag = async event => {
    const drag = explorer.drag; if (!drag || drag.pointerId !== event.pointerId) return;
    explorer.drag = null; if (chart.hasPointerCapture(event.pointerId)) chart.releasePointerCapture(event.pointerId);
    if (drag.button !== 2 || !explorer.selection) return;
    const selection = explorer.selection; explorer.selection = null; const width = Math.abs(selection.current.x - selection.start.x); const height = Math.abs(selection.current.y - selection.start.y);
    if (width < 3 || height < 3) { renderExplorer(); return; }
    const start = explorerCoordinates(selection.start, drag.startView); const end = explorerCoordinates(selection.current, drag.startView);
    await setExplorerView({ from: Math.min(start.time, end.time), to: Math.max(start.time, end.time), yMin: Math.min(start.value, end.value), yMax: Math.max(start.value, end.value) }, { showFailure: true }); renderExplorer();
  };
  chart.addEventListener('pointerup', endDrag); chart.addEventListener('pointercancel', endDrag);
}

async function renderSeries() {
  const list = $('#series-list'); const series = status.series || [];
  if (!series.length) { list.innerHTML = '<div class="empty">Aún no hay series configuradas. Elige un dispositivo y una lectura para empezar.</div>'; return; }
  list.innerHTML = ''; const hours = Number($('#hours-select').value);
  await Promise.all(series.map(async item => {
    const node = $('#series-template').content.firstElementChild.cloneNode(true); node.dataset.id = item.id; if (!item.enabled) node.classList.add('disabled');
    node.querySelector('h3').textContent = item.label; node.querySelector('.device-name').textContent = `${item.device_name || item.device_id} · ${item.metric}`;
    node.querySelector('.last-value').textContent = formatValue(item.last_value); node.querySelector('.unit').textContent = item.unit || '';
    const readingTime = node.querySelector('.reading-time'); readingTime.textContent = formatDate(item.last_seen_at); readingTime.title = 'Última captura confirmada';
    node.querySelector('.reading-count').textContent = `${item.reading_count} muestra${item.reading_count === 1 ? '' : 's'} confirmada${item.reading_count === 1 ? '' : 's'}`;
    const toggle = node.querySelector('.toggle'); toggle.textContent = item.enabled ? 'Pausar' : 'Reanudar'; toggle.onclick = () => updateSeries(item.id, { enabled: !Boolean(item.enabled) });
    node.querySelector('.expand').onclick = () => openExplorer(item); node.querySelector('.remove').onclick = () => removeSeries(item);
    try {
      const history = await request(`/api/history?seriesId=${item.id}&hours=${hours}`);
      node.querySelector('.chart').innerHTML = svgChart(history.readings, history.gaps, history.from, history.to);
      node.querySelector('.chart-note').textContent = history.gaps.length ? `${history.gaps.length} hueco${history.gaps.length === 1 ? '' : 's'} de captura` : (history.readings.length ? `${formatDate(history.readings[0].captured_at)} — ${formatDate(history.readings.at(-1).captured_at)}` : `Sin datos en ${hours} h`);
    } catch (error) { node.querySelector('.chart-note').textContent = error.message; }
    list.append(node);
  }));
}

const chartRefreshVersion = new Map();
async function refreshSeriesChart(seriesId, update) {
  const node = document.querySelector(`.series-card[data-id="${seriesId}"]`); if (!node) return;
  const version = (chartRefreshVersion.get(seriesId) || 0) + 1; chartRefreshVersion.set(seriesId, version);
  try {
    const history = await request(`/api/history?seriesId=${seriesId}&hours=${Number($('#hours-select').value)}`);
    if (chartRefreshVersion.get(seriesId) !== version) return;
    node.querySelector('.chart').innerHTML = svgChart(history.readings, history.gaps, history.from, history.to);
    node.querySelector('.chart-note').textContent = history.gaps.length ? `${history.gaps.length} hueco${history.gaps.length === 1 ? '' : 's'} de captura` : (history.readings.length ? `${formatDate(history.readings[0].captured_at)} — ${formatDate(history.readings.at(-1).captured_at)}` : 'Sin datos');
    node.querySelector('.reading-count').textContent = `${history.readingCount} muestra${history.readingCount === 1 ? '' : 's'} confirmada${history.readingCount === 1 ? '' : 's'}`;
    if (update.type === 'reading') { node.querySelector('.last-value').textContent = formatValue(update.value); node.querySelector('.unit').textContent = update.unit || ''; node.querySelector('.reading-time').textContent = formatDate(update.capturedAt); }
  } catch (error) { node.querySelector('.chart-note').textContent = error.message; }
}
async function refreshExplorerHistory(seriesId) {
  if (!explorer.active || explorer.active.id !== seriesId || !explorer.view) return;
  const view = { ...explorer.view }; explorer.readings.clear(); explorer.gaps.clear(); explorer.ranges = [];
  try { await loadExplorerRange(view.from, view.to); explorer.view = view; renderExplorer(); } catch (error) { setExplorerNotice(error.message, true); }
}

async function refresh({ charts = true } = {}) { status = await request('/api/status'); renderConnection(); renderDeviceOptions(); if (charts) await renderSeries(); }
async function updateSeries(id, body) { await request(`/api/series/${id}`, { method:'PATCH', headers:{'Content-Type':'application/json'}, body:JSON.stringify(body) }); await refresh(); }
async function removeSeries(item) { if (!confirm(`Se eliminará la serie «${item.label}» y todas sus muestras. ¿Continuar?`)) return; await request(`/api/series/${item.id}`, { method:'DELETE' }); await refresh(); }

$('#settings-form').onsubmit = async event => { event.preventDefault(); try { const data = await request('/api/settings', { method:'PUT', headers:{'Content-Type':'application/json'}, body:JSON.stringify({ gatewayUrl: $('#gateway-url').value }) }); $('#settings-message').textContent = `Conectando a ${data.gatewayUrl}`; await refresh(); } catch (error) { $('#settings-message').textContent = error.message; } };
$('#device-select').onchange = renderMetricOptions;
$('#metric-select').onchange = () => { const device = status.devices.find(item => item.device_id === $('#device-select').value); const metric = $('#metric-select').value; if (metric && !$('#label-input').value) $('#label-input').value = `${metric.replaceAll('_', ' ')} · ${device?.name || ''}`; };
$('#series-form').onsubmit = async event => { event.preventDefault(); try { await request('/api/series', { method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({ deviceId: $('#device-select').value, metric: $('#metric-select').value, label: $('#label-input').value }) }); $('#label-input').value = ''; await refresh(); } catch (error) { alert(error.message); } };
$('#hours-select').onchange = renderSeries;
setupExplorer();
refresh().catch(error => { $('#connection').className = 'connection error'; $('#connection').textContent = `● ${error.message}`; });
const historyEvents = new EventSource('/api/events');
historyEvents.addEventListener('history', event => {
  try { const update = JSON.parse(event.data); refreshSeriesChart(update.seriesId, update); refreshExplorerHistory(update.seriesId); } catch { /* El siguiente evento volverá a intentarlo. */ }
});
setInterval(() => refresh({ charts: false }).catch(() => {}), 5000);
