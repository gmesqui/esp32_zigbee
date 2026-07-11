import assert from 'node:assert/strict';
import { mkdtempSync, rmSync } from 'node:fs';
import { once } from 'node:events';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { spawn } from 'node:child_process';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

async function waitForServer(url, child) {
  for (let attempt = 0; attempt < 40; attempt++) {
    if (child.exitCode !== null) throw new Error(`El servidor de prueba terminó con ${child.exitCode}`);
    try {
      const response = await fetch(url);
      if (response.ok) return;
    } catch { /* Sigue arrancando. */ }
    await new Promise(resolve => setTimeout(resolve, 50));
  }
  throw new Error('El servidor de prueba no respondió a tiempo');
}

test('HTTP history API exposes confirmed readings and capture gaps', async () => {
  const directory = mkdtempSync(join(tmpdir(), 'zigbee-history-test-'));
  const port = 33000 + Math.floor(Math.random() * 1000);
  const child = spawn(process.execPath, ['server.js'], {
    cwd: fileURLToPath(new URL('../', import.meta.url)),
    env: {
      ...process.env,
      PORT: String(port),
      ZIGBEE_HISTORY_DB: join(directory, 'history.sqlite'),
      ZIGBEE_HISTORY_GATEWAY_URL: 'ws://127.0.0.1:1/ws',
    },
    stdio: 'ignore',
  });
  const base = `http://127.0.0.1:${port}`;
  try {
    await waitForServer(`${base}/api/status`, child);
    const created = await fetch(`${base}/api/series`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ deviceId: 'test-sensor', metric: 'temperature', label: 'Temperatura' }),
    });
    assert.equal(created.status, 201);
    const status = await (await fetch(`${base}/api/status`)).json();
    const seriesId = status.series[0].id;
    const response = await fetch(`${base}/api/history?seriesId=${seriesId}&hours=1`);
    assert.equal(response.status, 200);
    const history = await response.json();
    assert.deepEqual(history.readings, []);
    assert.ok(history.gaps.some(gap => gap.reason === 'awaiting_first_sample'));
    assert.ok(history.gaps.some(gap => gap.reason === 'gateway_disconnected'));
  } finally {
    if (child.exitCode === null) {
      child.kill('SIGTERM');
      await once(child, 'exit');
    }
    rmSync(directory, { recursive: true, force: true });
  }
});
