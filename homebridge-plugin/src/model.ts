import type { PlatformConfig } from 'homebridge';

export const PLUGIN_NAME = 'homebridge-esp32-zigbee';
export const PLATFORM_NAME = 'ESP32ZigbeeBridge';
export const WS_PROTOCOL_VERSION = 1;

export interface ZigbeePlatformConfig extends PlatformConfig {
  url?: string;
  host?: string;
  port?: number;
  path?: string;
  tls?: boolean;
  pingIntervalSeconds?: number;
  reconnectDelaySeconds?: number;
  commandTimeoutSeconds?: number;
}

export interface AttributeValue {
  value: boolean | number | string;
  unit?: string;
  ts?: number;
  quality?: string;
}

export interface InventoryDevice {
  device_id: string;
  name: string;
  manufacturer?: string;
  model?: string;
  power_source?: string;
  capabilities: string[];
}

export interface DeviceMetaState {
  reachable?: boolean;
  last_seen?: number;
  link_quality?: number;
}

export interface StateDeviceSnapshot {
  device_id: string;
  meta?: DeviceMetaState;
  state?: Record<string, AttributeValue>;
}

export interface ZigbeeEventMessage {
  device_id: string | null;
  changes?: Record<string, AttributeValue>;
  event?: string;
  value?: boolean;
  duration?: number;
  name?: string;
  status?: string;
}

export interface CmdResultMessage {
  status: string;
  applied: boolean;
  error?: string;
}

export interface ErrorMessage {
  code: string;
  message: string;
}

export interface PersistedAccessoryContext {
  device?: InventoryDevice;
  lastKnown?: StateDeviceSnapshot;
}

const PRIMARY_HOMEKIT_CAPABILITIES = new Set([
  'switch',
  'temperature_sensor',
  'humidity_sensor',
  'illuminance_sensor',
  'occupancy_sensor',
  'ias_zone_sensor',
]);

export function buildWebSocketUrl(config: ZigbeePlatformConfig): string {
  const directUrl = config.url?.trim();
  if (directUrl) {
    return directUrl;
  }

  const rawHost = config.host?.trim() || 'esp32-zigbee.local';
  if (rawHost.startsWith('ws://') || rawHost.startsWith('wss://')) {
    return rawHost;
  }

  const port = Number.isFinite(config.port) ? Number(config.port) : 8080;
  let path = config.path?.trim() || '/ws';
  if (!path.startsWith('/')) {
    path = `/${path}`;
  }

  const scheme = config.tls ? 'wss' : 'ws';
  return `${scheme}://${rawHost}:${port}${path}`;
}

export function deviceKey(deviceId: string): string {
  return deviceId.trim().toLowerCase();
}

export function normalizeInventoryDevice(device: InventoryDevice): InventoryDevice {
  return {
    ...device,
    device_id: device.device_id.trim(),
    name: device.name?.trim() || device.device_id,
    manufacturer: device.manufacturer?.trim() || 'Unknown',
    model: device.model?.trim() || 'Unknown',
    power_source: device.power_source?.trim() || 'unknown',
    capabilities: Array.isArray(device.capabilities)
      ? [...new Set(device.capabilities.map(capability => String(capability).trim()).filter(Boolean))]
      : [],
  };
}

export function normalizeStateSnapshot(snapshot: StateDeviceSnapshot): StateDeviceSnapshot {
  return {
    device_id: snapshot.device_id.trim(),
    meta: snapshot.meta ?? {},
    state: snapshot.state ?? {},
  };
}

export function supportsHomeKitPrimaryService(device: InventoryDevice): boolean {
  return device.capabilities.some(capability => PRIMARY_HOMEKIT_CAPABILITIES.has(capability));
}

export function supportsCapability(device: InventoryDevice | undefined, capability: string): boolean {
  return !!device && device.capabilities.includes(capability);
}

export function clampNumber(value: number, min: number, max: number): number {
  return Math.min(max, Math.max(min, value));
}
