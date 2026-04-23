import type {
  API,
  DynamicPlatformPlugin,
  Logger,
  PlatformAccessory,
} from 'homebridge';

import { GatewayClient } from './client';
import { ZigbeeDeviceAccessory } from './accessory';
import {
  InventoryDevice,
  PersistedAccessoryContext,
  PLATFORM_NAME,
  PLUGIN_NAME,
  StateDeviceSnapshot,
  ZigbeeEventMessage,
  ZigbeePlatformConfig,
  buildWebSocketUrl,
  deviceKey,
  normalizeInventoryDevice,
  normalizeStateSnapshot,
  supportsHomeKitPrimaryService,
} from './model';

type ZigbeePlatformAccessory = PlatformAccessory<PersistedAccessoryContext>;

export class Esp32ZigbeePlatform implements DynamicPlatformPlugin {
  private readonly accessories = new Map<string, ZigbeeDeviceAccessory>();
  private readonly cachedAccessories = new Map<string, ZigbeePlatformAccessory>();
  private readonly client: GatewayClient;
  private gatewayAvailable = false;
  private resyncTimer?: NodeJS.Timeout;

  public get Service() {
    return this.api.hap.Service;
  }

  public get Characteristic() {
    return this.api.hap.Characteristic;
  }

  constructor(
    public readonly log: Logger,
    public readonly config: ZigbeePlatformConfig,
    public readonly api: API,
  ) {
    const url = buildWebSocketUrl(config);
    this.client = new GatewayClient(this.log, {
      url,
      pingIntervalMs: Math.max(5, config.pingIntervalSeconds ?? 25) * 1000,
      reconnectDelayMs: Math.max(2, config.reconnectDelaySeconds ?? 5) * 1000,
      commandTimeoutMs: Math.max(3, config.commandTimeoutSeconds ?? 10) * 1000,
    });

    this.bindClientEvents();

    this.api.on('didFinishLaunching', () => {
      this.log.info(`Inicializando plataforma ${PLATFORM_NAME} hacia ${url}`);
      this.client.start();
    });
  }

  public configureAccessory(accessory: ZigbeePlatformAccessory): void {
    const deviceId = accessory.context.device?.device_id ?? accessory.context.lastKnown?.device_id;
    if (!deviceId) {
      this.log.warn(`Accesorio en cache sin device_id, se omite: ${accessory.displayName}`);
      return;
    }

    const key = deviceKey(deviceId);
    this.cachedAccessories.set(key, accessory);
    this.accessories.set(key, new ZigbeeDeviceAccessory(this, accessory));
  }

  public async sendOnOffCommand(deviceId: string, state: boolean): Promise<void> {
    await this.client.setOnOff(deviceId, state);
  }

  private bindClientEvents(): void {
    this.client.on('connected', () => {
      this.gatewayAvailable = true;
      for (const accessory of this.accessories.values()) {
        accessory.setGatewayAvailable(true);
      }
    });

    this.client.on('disconnected', reason => {
      this.gatewayAvailable = false;
      if (reason) {
        this.log.warn(`Gateway no disponible: ${reason}`);
      }
      for (const accessory of this.accessories.values()) {
        accessory.setGatewayAvailable(false);
      }
    });

    this.client.on('hello', payload => {
      const session = typeof payload.session_id === 'string' ? payload.session_id : 'unknown';
      this.log.info(`Sesion WS activa: ${session}`);
    });

    this.client.on('inventory', (devices, generation) => {
      this.log.info(`Inventario recibido: ${devices.length} dispositivos (generation=${generation})`);
      this.reconcileInventory(devices);
    });

    this.client.on('state', (devices, generation) => {
      this.log.debug(`Snapshot de estado recibido: ${devices.length} dispositivos (generation=${generation})`);
      for (const snapshot of devices) {
        this.applyStateSnapshot(snapshot);
      }
    });

    this.client.on('event', (messageType, payload) => {
      this.handleGatewayEvent(messageType, payload);
    });
  }

  private reconcileInventory(devices: InventoryDevice[]): void {
    const seen = new Set<string>();

    for (const rawDevice of devices) {
      const device = normalizeInventoryDevice(rawDevice);
      const key = deviceKey(device.device_id);

      if (!supportsHomeKitPrimaryService(device)) {
        this.log.debug(`Se omite ${device.name} (${device.device_id}) porque no tiene servicios HomeKit primarios soportados`);
        continue;
      }

      seen.add(key);
      const existing = this.accessories.get(key);

      if (existing) {
        existing.updateInventory(device);
        existing.setGatewayAvailable(this.gatewayAvailable);
        this.api.updatePlatformAccessories([existing.accessory]);
        continue;
      }

      const uuid = this.api.hap.uuid.generate(device.device_id);
      const cached = this.cachedAccessories.get(key);
      const accessory = cached ?? new this.api.platformAccessory<PersistedAccessoryContext>(device.name, uuid);
      accessory.context.device = device;
      accessory.context.lastKnown ??= {
        device_id: device.device_id,
        meta: {},
        state: {},
      };

      const wrapper = new ZigbeeDeviceAccessory(this, accessory);
      wrapper.updateInventory(device);
      wrapper.setGatewayAvailable(this.gatewayAvailable);

      this.accessories.set(key, wrapper);

      if (cached) {
        this.cachedAccessories.delete(key);
        this.api.updatePlatformAccessories([accessory]);
      } else {
        this.api.registerPlatformAccessories(PLUGIN_NAME, PLATFORM_NAME, [accessory]);
      }
    }

    for (const [key, wrapper] of [...this.accessories.entries()]) {
      if (seen.has(key)) {
        continue;
      }

      this.log.info(`Eliminando accesorio ausente en inventario: ${wrapper.accessory.displayName}`);
      this.accessories.delete(key);
      this.cachedAccessories.delete(key);
      this.api.unregisterPlatformAccessories(PLUGIN_NAME, PLATFORM_NAME, [wrapper.accessory]);
    }
  }

  private applyStateSnapshot(snapshot: StateDeviceSnapshot): void {
    const normalized = normalizeStateSnapshot(snapshot);
    const accessory = this.accessories.get(deviceKey(normalized.device_id));
    if (!accessory) {
      return;
    }

    accessory.applyStateSnapshot(normalized);
    accessory.setGatewayAvailable(this.gatewayAvailable);
    this.api.updatePlatformAccessories([accessory.accessory]);
  }

  private handleGatewayEvent(messageType: string, payload: ZigbeeEventMessage): void {
    if (!payload.device_id) {
      if (payload.event === 'permit_join') {
        this.log.info(`Permit join ${payload.value ? 'abierto' : 'cerrado'} (${payload.duration ?? 0}s)`);
      }
      return;
    }

    const key = deviceKey(payload.device_id);
    const accessory = this.accessories.get(key);

    if (messageType === 'event' && payload.changes?.reachable && accessory) {
      accessory.applyReachability({
        reachable: Boolean(payload.changes.reachable.value),
      });
      accessory.setGatewayAvailable(this.gatewayAvailable);
      this.api.updatePlatformAccessories([accessory.accessory]);
      return;
    }

    if (messageType === 'event' && payload.changes && accessory) {
      accessory.applyEventChanges(payload.changes);
      accessory.setGatewayAvailable(this.gatewayAvailable);
      this.api.updatePlatformAccessories([accessory.accessory]);
      return;
    }

    if (messageType === 'device_updated' && accessory && payload.name) {
      const current = accessory.accessory.context.device;
      if (current) {
        accessory.updateInventory({
          ...current,
          name: payload.name,
        });
        this.api.updatePlatformAccessories([accessory.accessory]);
      }
      this.scheduleResync('device_updated');
      return;
    }

    if (messageType === 'device_joined' || messageType === 'device_left' || messageType === 'device_updated') {
      this.scheduleResync(messageType);
    }
  }

  private scheduleResync(reason: string): void {
    if (this.resyncTimer) {
      return;
    }

    this.log.debug(`Programando resync por ${reason}`);
    this.resyncTimer = setTimeout(() => {
      this.resyncTimer = undefined;
      if (this.gatewayAvailable) {
        this.client.requestResync();
      }
    }, 1500);
  }
}
