import assert from 'node:assert/strict';
import { mkdtempSync, rmSync } from 'node:fs';
import { once } from 'node:events';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { spawn } from 'node:child_process';
import { gzipSync } from 'node:zlib';
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

test('HTTP history API exposes confirmed readings and capture gaps', async t => {
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

    await t.test('compressed JSON works and trailing invalid bytes do not stop the server', async () => {
      const payload = gzipSync(JSON.stringify({ deviceId: 'compressed-sensor', metric: 'humidity' }));
      const compressed = await fetch(`${base}/api/series`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', 'Content-Encoding': 'gzip' },
        body: payload,
        signal: AbortSignal.timeout(5_000),
      });
      assert.equal(compressed.status, 201);
      await compressed.arrayBuffer();

      const malformed = await fetch(`${base}/api/series`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', 'Content-Encoding': 'gzip' },
        body: Buffer.concat([payload, Buffer.from([0xde, 0xad, 0xbe, 0xef])]),
        signal: AbortSignal.timeout(5_000),
      });
      assert.equal(malformed.status, 400);
      await malformed.arrayBuffer();

      // GHSA-27r3-jxmh-p828: a completed gzip frame, a zero byte, then
      // enough trailing data to reach the decoder after its readable end.
      const trailing = await fetch(`${base}/api/series`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', 'Content-Encoding': 'gzip' },
        body: Buffer.concat([payload, Buffer.from([0]), Buffer.alloc(100 * 1024, 0x58)]),
        signal: AbortSignal.timeout(5_000),
      });
      assert.equal(trailing.status, 201);
      await trailing.arrayBuffer();
      // The vulnerable decoder can throw after the successful HTTP response.
      await new Promise(resolve => setTimeout(resolve, 100));

      const recovered = await fetch(`${base}/api/series`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ deviceId: 'after-malformed-sensor', metric: 'temperature' }),
        signal: AbortSignal.timeout(5_000),
      });
      assert.equal(recovered.status, 201);
      await recovered.arrayBuffer();
      assert.equal(child.exitCode, null);
    });
  } finally {
    if (child.exitCode === null) {
      child.kill('SIGTERM');
      await once(child, 'exit');
    }
    rmSync(directory, { recursive: true, force: true });
  }
});
