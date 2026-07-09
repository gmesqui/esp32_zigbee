let status = null;
const $ = selector => document.querySelector(selector);
const escapeHtml = value => String(value ?? '').replace(/[&<>'"]/g, char => ({ '&':'&amp;', '<':'&lt;', '>':'&gt;', "'":'&#39;', '"':'&quot;' }[char]));
const formatValue = value => typeof value === 'boolean' ? (value ? 'Sí' : 'No') : typeof value === 'object' ? JSON.stringify(value) : String(value ?? '—');
const formatDate = time => time ? new Intl.DateTimeFormat('es-ES', { dateStyle:'short', timeStyle:'short' }).format(time) : 'sin lectura';

async function request(url, options) { const response = await fetch(url, options); const data = response.status === 204 ? null : await response.json(); if (!response.ok) throw new Error(data?.error || 'No se pudo completar la operación.'); return data; }
function attributesFor(deviceId) { return (status?.attributes || []).filter(attribute => attribute.device_id === deviceId); }

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

function svgChart(readings) {
  const values = readings.filter(item => Number.isFinite(item.numeric_value));
  if (!values.length) return '<text x="400" y="125" text-anchor="middle" fill="#6b7783" font-size="16">Aún no hay valores numéricos para representar</text>';
  const minX = values[0].captured_at, maxX = values.at(-1).captured_at === minX ? minX + 1 : values.at(-1).captured_at;
  const rawMin = Math.min(...values.map(item => item.numeric_value)), rawMax = Math.max(...values.map(item => item.numeric_value));
  const margin = rawMin === rawMax ? Math.max(Math.abs(rawMin) * .05, 1) : (rawMax - rawMin) * .08; const minY = rawMin - margin, maxY = rawMax + margin;
  const x = value => 36 + ((value - minX) / (maxX - minX)) * 752; const y = value => 218 - ((value - minY) / (maxY - minY)) * 190;
  const points = values.map(item => `${x(item.captured_at).toFixed(1)},${y(item.numeric_value).toFixed(1)}`).join(' ');
  return `<line class="grid" x1="36" y1="28" x2="788" y2="28"/><line class="grid" x1="36" y1="123" x2="788" y2="123"/><line class="grid" x1="36" y1="218" x2="788" y2="218"/><text x="2" y="33" fill="#6b7783" font-size="12">${rawMax.toFixed(2)}</text><text x="2" y="222" fill="#6b7783" font-size="12">${rawMin.toFixed(2)}</text><polyline class="line" points="${points}"/>${values.length < 70 ? values.map(item => `<circle class="dot" cx="${x(item.captured_at).toFixed(1)}" cy="${y(item.numeric_value).toFixed(1)}" r="3"/>`).join('') : ''}`;
}

async function renderSeries() {
  const list = $('#series-list'); const series = status.series || [];
  if (!series.length) { list.innerHTML = '<div class="empty">Aún no hay series configuradas. Elige un dispositivo y una lectura para empezar.</div>'; return; }
  list.innerHTML = '';
  const hours = Number($('#hours-select').value);
  await Promise.all(series.map(async item => {
    const node = $('#series-template').content.firstElementChild.cloneNode(true); node.dataset.id = item.id; if (!item.enabled) node.classList.add('disabled');
    node.querySelector('h3').textContent = item.label; node.querySelector('.device-name').textContent = `${item.device_name || item.device_id} · ${item.metric}`;
    node.querySelector('.last-value').textContent = formatValue(item.last_value); node.querySelector('.unit').textContent = item.unit || '';
    node.querySelector('.reading-time').textContent = formatDate(item.last_seen_at); node.querySelector('.reading-count').textContent = `${item.reading_count} muestra${item.reading_count === 1 ? '' : 's'} guardada${item.reading_count === 1 ? '' : 's'}`;
    const toggle = node.querySelector('.toggle'); toggle.textContent = item.enabled ? 'Pausar' : 'Reanudar'; toggle.onclick = () => updateSeries(item.id, { enabled: !Boolean(item.enabled) });
    node.querySelector('.remove').onclick = () => removeSeries(item);
    try { const history = await request(`/api/history?seriesId=${item.id}&hours=${hours}`); node.querySelector('.chart').innerHTML = svgChart(history.readings); node.querySelector('.chart-note').textContent = history.readings.length ? `${formatDate(history.readings[0].captured_at)} — ${formatDate(history.readings.at(-1).captured_at)}` : `Sin datos en ${hours} h`; }
    catch (error) { node.querySelector('.chart-note').textContent = error.message; }
    list.append(node);
  }));
}

const chartRefreshVersion = new Map();

async function refreshSeriesChart(seriesId, update) {
  const node = document.querySelector(`.series-card[data-id="${seriesId}"]`);
  if (!node) return;
  const version = (chartRefreshVersion.get(seriesId) || 0) + 1;
  chartRefreshVersion.set(seriesId, version);
  const hours = Number($('#hours-select').value);
  try {
    const history = await request(`/api/history?seriesId=${seriesId}&hours=${hours}`);
    if (chartRefreshVersion.get(seriesId) !== version) return;
    node.querySelector('.chart').innerHTML = svgChart(history.readings);
    node.querySelector('.chart-note').textContent = history.readings.length ? `${formatDate(history.readings[0].captured_at)} — ${formatDate(history.readings.at(-1).captured_at)}` : `Sin datos en ${hours} h`;
    node.querySelector('.last-value').textContent = formatValue(update.value);
    node.querySelector('.unit').textContent = update.unit || '';
    node.querySelector('.reading-time').textContent = formatDate(update.capturedAt);
    node.querySelector('.reading-count').textContent = `${history.readingCount} muestra${history.readingCount === 1 ? '' : 's'} guardada${history.readingCount === 1 ? '' : 's'}`;
  } catch (error) {
    node.querySelector('.chart-note').textContent = error.message;
  }
}

async function refresh({ charts = true } = {}) { status = await request('/api/status'); renderConnection(); renderDeviceOptions(); if (charts) await renderSeries(); }
async function updateSeries(id, body) { await request(`/api/series/${id}`, { method:'PATCH', headers:{'Content-Type':'application/json'}, body:JSON.stringify(body) }); await refresh(); }
async function removeSeries(item) { if (!confirm(`Se eliminará la serie «${item.label}» y todas sus muestras. ¿Continuar?`)) return; await request(`/api/series/${item.id}`, { method:'DELETE' }); await refresh(); }

$('#settings-form').onsubmit = async event => { event.preventDefault(); try { const data = await request('/api/settings', { method:'PUT', headers:{'Content-Type':'application/json'}, body:JSON.stringify({ gatewayUrl: $('#gateway-url').value }) }); $('#settings-message').textContent = `Conectando a ${data.gatewayUrl}`; await refresh(); } catch (error) { $('#settings-message').textContent = error.message; } };
$('#device-select').onchange = renderMetricOptions;
$('#metric-select').onchange = () => { const device = status.devices.find(item => item.device_id === $('#device-select').value); const metric = $('#metric-select').value; if (metric && !$('#label-input').value) $('#label-input').value = `${metric.replaceAll('_', ' ')} · ${device?.name || ''}`; };
$('#series-form').onsubmit = async event => { event.preventDefault(); try { await request('/api/series', { method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({ deviceId: $('#device-select').value, metric: $('#metric-select').value, label: $('#label-input').value }) }); $('#label-input').value = ''; await refresh(); } catch (error) { alert(error.message); } };
$('#hours-select').onchange = renderSeries;
refresh().catch(error => { $('#connection').className = 'connection error'; $('#connection').textContent = `● ${error.message}`; });
const historyEvents = new EventSource('/api/events');
historyEvents.addEventListener('reading', event => {
  try {
    const update = JSON.parse(event.data);
    refreshSeriesChart(update.seriesId, update);
  } catch { /* El siguiente evento volverá a intentarlo. */ }
});
setInterval(() => refresh({ charts: false }).catch(() => {}), 5000);
