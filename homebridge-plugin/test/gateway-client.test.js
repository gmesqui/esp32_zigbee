const assert = require('node:assert/strict');
const test = require('node:test');
const { once } = require('node:events');
const { WebSocketServer } = require('ws');
const hap = require('@homebridge/hap-nodejs');
const { PlatformAccessory } = require('../node_modules/homebridge/dist/platformAccessory');

const { GatewayClient } = require('../dist/client');
const { Esp32ZigbeePlatform } = require('../dist/platform');

function logger() {
  return { info() {}, warn() {}, debug() {} };
}

async function createGateway() {
  const server = new WebSocketServer({ port: 0 });
  await once(server, 'listening');
  const { port } = server.address();
  return { server, url: `ws://127.0.0.1:${port}/ws` };
}

function clientFor(url) {
  return new GatewayClient(logger(), {
    url,
    pingIntervalMs: 60_000,
    reconnectDelayMs: 25,
    commandTimeoutMs: 100,
  });
}

function commandQueue(socket) {
  const messages = [];
  const waiters = [];
  socket.on('message', raw => {
    const message = JSON.parse(raw.toString());
    const waiter = waiters.shift();
    if (waiter) {
      waiter(message);
    } else {
      messages.push(message);
    }
  });

  async function nextMessage() {
    return messages.shift() ?? new Promise(resolve => waiters.push(resolve));
  }

  return {
    async nextCommand() {
      for (;;) {
        const message = await nextMessage();
        if (message.type === 'cmd') {
          return message;
        }
      }
    },
  };
}

async function closeGateway(server) {
  for (const socket of server.clients) {
    socket.terminate();
  }
  await new Promise(resolve => server.close(resolve));
}

test('reconstruye un inventario fragmentado una sola vez', { timeout: 2_000 }, async t => {
  const { server, url } = await createGateway();
  const client = clientFor(url);
  t.after(async () => {
    client.stop();
    await closeGateway(server);
  });
  const inventory = [];
  client.on('inventory', devices => inventory.push(devices));
  const connection = once(server, 'connection');
  client.start();
  const [socket] = await connection;

  socket.send(JSON.stringify({ type: 'inventory_chunk', msg_id: 1, data: {
    stream_id: 'inventory-1', generation: 7, index: 1, final: true,
    devices: [{ device_id: 'two', name: 'Two', capabilities: ['switch'] }],
  } }));
  socket.send(JSON.stringify({ type: 'inventory_chunk', msg_id: 2, data: {
    stream_id: 'inventory-1', generation: 7, index: 0, final: false,
    devices: [{ device_id: 'one', name: 'One', capabilities: ['switch'] }],
  } }));

  await new Promise(resolve => setTimeout(resolve, 25));
  assert.deepEqual(inventory, [[
    { device_id: 'one', name: 'One', capabilities: ['switch'] },
    { device_id: 'two', name: 'Two', capabilities: ['switch'] },
  ]]);
});

test('resuelve, rechaza, expira y cancela comandos on/off', { timeout: 2_000 }, async t => {
  const { server, url } = await createGateway();
  const client = clientFor(url);
  t.after(async () => {
    client.stop();
    await closeGateway(server);
  });
  const connection = once(server, 'connection');
  client.start();
  const [socket] = await connection;
  const commands = commandQueue(socket);
  await once(client, 'connected');

  const accepted = client.setOnOff('device-1', true);
  const request = await commands.nextCommand();
  socket.send(JSON.stringify({ type: 'cmd_result', msg_id: 3, reply_to: request.msg_id, data: { status: 'ok', applied: true } }));
  await assert.doesNotReject(accepted);

  const rejected = client.setOnOff('device-1', false);
  const rejectedRequest = await commands.nextCommand();
  socket.send(JSON.stringify({ type: 'cmd_result', msg_id: 4, reply_to: rejectedRequest.msg_id, data: { status: 'error', applied: false, error: 'denied' } }));
  await assert.rejects(rejected, /denied/);

  const timedOut = client.setOnOff('device-1', true);
  await commands.nextCommand();
  await assert.rejects(timedOut, /timeout/);

  const stopped = client.setOnOff('device-1', true);
  await commands.nextCommand();
  client.stop();
  await assert.rejects(stopped, /stopped/);
});

test('conserva los accesorios y aplica reachability y sensores del mismo evento', () => {
  const listeners = new Map();
  const updated = [];
  const api = {
    hap,
    platformAccessory: hap.Accessory,
    on(event, listener) {
      listeners.set(event, listener);
      return this;
    },
    registerPlatformAccessories() {},
    unregisterPlatformAccessories() {},
    updatePlatformAccessories(accessories) {
      updated.push(...accessories);
    },
  };
  const device = {
    device_id: '0x00124b0000000001',
    name: 'Kitchen Sensor',
    capabilities: ['temperature_sensor'],
  };
  const accessory = new hap.Accessory(device.name, hap.uuid.generate(device.device_id));
  accessory.context = { device, lastKnown: { device_id: device.device_id, meta: {}, state: {} } };
  const platform = new Esp32ZigbeePlatform(logger(), { name: 'ESP32 Zigbee Bridge' }, api);

  platform.configureAccessory(accessory);
  platform.client.emit('connected');
  platform.client.emit('inventory', [device], 1);
  platform.client.emit('event', 'event', {
    device_id: device.device_id,
    changes: {
      reachable: { value: true },
      temperature: { value: 21.5, unit: '°C' },
    },
  });

  const service = accessory.getServiceById(hap.Service.TemperatureSensor, 'temperature');
  assert.ok(service);
  assert.equal(service.getCharacteristic(hap.Characteristic.CurrentTemperature).value, 21.5);
  assert.equal(accessory.context.lastKnown.meta.reachable, true);
  assert.equal(accessory.context.lastKnown.state.temperature.value, 21.5);
  assert.ok(updated.length >= 2);
  listeners.get('shutdown')();
});

test('sincroniza nombres de accesorio y servicios al restaurar y renombrar', () => {
  const listeners = new Map();
  const updated = [];
  const api = {
    hap,
    platformAccessory: PlatformAccessory,
    on(event, listener) {
      listeners.set(event, listener);
      return this;
    },
    registerPlatformAccessories() {},
    unregisterPlatformAccessories() {},
    updatePlatformAccessories(accessories) {
      updated.push(...accessories);
    },
  };
  const oldDevice = {
    device_id: '0x00124b0000000002',
    name: '0x00124b0000000002',
    capabilities: ['switch', 'battery_sensor'],
    power_source: 'battery',
  };
  const newDevice = { ...oldDevice, name: 'Lámpara salón' };
  const accessory = new PlatformAccessory(oldDevice.name, hap.uuid.generate(oldDevice.device_id));
  accessory.context = {
    device: oldDevice,
    lastKnown: { device_id: oldDevice.device_id, meta: {}, state: {} },
  };
  const uuid = accessory.UUID;
  const platform = new Esp32ZigbeePlatform(logger(), { name: 'ESP32 Zigbee Bridge' }, api);

  platform.configureAccessory(accessory);
  platform.client.emit('connected');
  platform.client.emit('inventory', [newDevice], 1);

  const serviceIds = accessory.services.map(service => `${service.UUID}:${service.subtype ?? ''}`);
  const switchService = accessory.getServiceById(hap.Service.Switch, 'switch');
  const batteryService = accessory.getServiceById(hap.Service.Battery, 'battery');
  assert.equal(accessory.UUID, uuid);
  assert.equal(accessory.displayName, 'Lámpara salón');
  assert.equal(accessory._associatedHAPAccessory.displayName, 'Lámpara salón');
  assert.equal(switchService.displayName, 'Lámpara salón');
  assert.equal(switchService.getCharacteristic(hap.Characteristic.Name).value, 'Lámpara salón');
  assert.equal(batteryService.displayName, 'Lámpara salón Battery');
  assert.equal(batteryService.getCharacteristic(hap.Characteristic.Name).value, 'Lámpara salón Battery');

  platform.client.emit('event', 'device_updated', {
    device_id: newDevice.device_id,
    name: 'Luz salón',
  });

  assert.equal(accessory.displayName, 'Luz salón');
  assert.equal(switchService.displayName, 'Luz salón');
  assert.equal(switchService.getCharacteristic(hap.Characteristic.Name).value, 'Luz salón');
  assert.equal(batteryService.displayName, 'Luz salón Battery');
  assert.equal(batteryService.getCharacteristic(hap.Characteristic.Name).value, 'Luz salón Battery');
  assert.deepEqual(accessory.services.map(service => `${service.UUID}:${service.subtype ?? ''}`), serviceIds);
  assert.equal(accessory.context.device.name, 'Luz salón');
  assert.ok(updated.length >= 2);
  listeners.get('shutdown')();
});
