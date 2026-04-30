import type { CharacteristicValue, PlatformAccessory, Service } from 'homebridge';

import type { Esp32ZigbeePlatform } from './platform';
import {
  AttributeValue,
  DeviceMetaState,
  InventoryDevice,
  PersistedAccessoryContext,
  StateDeviceSnapshot,
  clampNumber,
  deviceKey,
  normalizeInventoryDevice,
  normalizeStateSnapshot,
  supportsBatteryService,
  supportsCapability,
} from './model';

type ZigbeePlatformAccessory = PlatformAccessory<PersistedAccessoryContext>;
type ServiceConstructor = {
  new (displayName?: string, subtype?: string): Service;
  UUID: string;
};

const SERVICE_IDS = {
  switch: 'switch',
  temperature: 'temperature',
  humidity: 'humidity',
  illuminance: 'illuminance',
  occupancy: 'occupancy',
  contact: 'contact',
  battery: 'battery',
} as const;

export class ZigbeeDeviceAccessory {
  private device?: InventoryDevice;
  private gatewayAvailable = false;

  constructor(
    private readonly platform: Esp32ZigbeePlatform,
    public readonly accessory: ZigbeePlatformAccessory,
  ) {
    this.device = accessory.context.device;
    this.syncAccessory();
  }

  public get deviceId(): string | undefined {
    return this.device?.device_id;
  }

  public matchesDevice(deviceId: string): boolean {
    return !!this.device && deviceKey(this.device.device_id) === deviceKey(deviceId);
  }

  public updateInventory(device: InventoryDevice): void {
    this.device = normalizeInventoryDevice(device);
    this.accessory.context.device = this.device;

    if (!this.accessory.context.lastKnown || deviceKey(this.accessory.context.lastKnown.device_id) !== deviceKey(this.device.device_id)) {
      this.accessory.context.lastKnown = {
        device_id: this.device.device_id,
        meta: {},
        state: {},
      };
    }

    this.syncAccessory();
  }

  public applyStateSnapshot(snapshot: StateDeviceSnapshot): void {
    const normalized = normalizeStateSnapshot(snapshot);
    this.ensureLastKnown(normalized.device_id);

    const current = this.accessory.context.lastKnown!;
    current.meta = normalized.meta ?? {};
    current.state = normalized.state ?? {};

    this.applyMeta(current.meta);
    this.applyState(current.state);
  }

  public applyEventChanges(changes: Record<string, AttributeValue>): void {
    if (!this.device) {
      return;
    }

    this.ensureLastKnown(this.device.device_id);
    const state = this.accessory.context.lastKnown!.state ?? {};
    this.accessory.context.lastKnown!.state = {
      ...state,
      ...changes,
    };

    this.applyState(this.accessory.context.lastKnown!.state ?? {});
  }

  public applyReachability(meta: DeviceMetaState): void {
    if (!this.device) {
      return;
    }

    this.ensureLastKnown(this.device.device_id);
    this.accessory.context.lastKnown!.meta = {
      ...this.accessory.context.lastKnown!.meta,
      ...meta,
    };

    this.applyMeta(this.accessory.context.lastKnown!.meta ?? {});
  }

  public setGatewayAvailable(available: boolean): void {
    this.gatewayAvailable = available;
    this.updateStatusFaults();
  }

  private syncAccessory(): void {
    if (!this.device) {
      return;
    }

    this.accessory.displayName = this.device.name;
    this.accessory.context.device = this.device;
    this.ensureLastKnown(this.device.device_id);

    const info = this.accessory.getService(this.platform.Service.AccessoryInformation)
      ?? this.accessory.addService(this.platform.Service.AccessoryInformation);

    info
      .setCharacteristic(this.platform.Characteristic.Name, this.device.name)
      .setCharacteristic(this.platform.Characteristic.Manufacturer, this.device.manufacturer ?? 'Unknown')
      .setCharacteristic(this.platform.Characteristic.Model, this.device.model ?? 'Unknown')
      .setCharacteristic(this.platform.Characteristic.SerialNumber, this.device.device_id);

    this.configureSwitchService();
    this.configureSensorServices();
    this.configureBatteryService();
    this.removeStaleServices();

    const lastKnown = this.accessory.context.lastKnown;
    if (lastKnown) {
      this.applyMeta(lastKnown.meta ?? {});
      this.applyState(lastKnown.state ?? {});
    }
  }

  private configureSwitchService(): void {
    if (!supportsCapability(this.device, 'switch')) {
      this.removeServiceIfPresent(this.platform.Service.Switch.UUID, SERVICE_IDS.switch);
      return;
    }

    const service = this.ensureService(this.platform.Service.Switch, this.device!.name, SERVICE_IDS.switch);
    if (!service.testCharacteristic(this.platform.Characteristic.On)) {
      service.addCharacteristic(this.platform.Characteristic.On);
    }

    service.getCharacteristic(this.platform.Characteristic.On)
      .onGet(() => this.getOnValue())
      .onSet(async value => this.setOnValue(value));
  }

  private configureSensorServices(): void {
    this.configureTemperatureService();
    this.configureHumidityService();
    this.configureIlluminanceService();
    this.configureOccupancyService();
    this.configureContactService();
  }

  private configureTemperatureService(): void {
    this.configureOptionalService(
      'temperature_sensor',
      this.platform.Service.TemperatureSensor,
      SERVICE_IDS.temperature,
    );
  }

  private configureHumidityService(): void {
    this.configureOptionalService(
      'humidity_sensor',
      this.platform.Service.HumiditySensor,
      SERVICE_IDS.humidity,
    );
  }

  private configureIlluminanceService(): void {
    this.configureOptionalService(
      'illuminance_sensor',
      this.platform.Service.LightSensor,
      SERVICE_IDS.illuminance,
    );
  }

  private configureOccupancyService(): void {
    this.configureOptionalService(
      'occupancy_sensor',
      this.platform.Service.OccupancySensor,
      SERVICE_IDS.occupancy,
    );
  }

  private configureContactService(): void {
    this.configureOptionalService(
      'ias_zone_sensor',
      this.platform.Service.ContactSensor,
      SERVICE_IDS.contact,
    );
  }

  private configureBatteryService(): void {
    const shouldExposeBattery = supportsBatteryService(this.device) && this.hasPrimaryService();
    if (!shouldExposeBattery) {
      this.removeServiceIfPresent(this.platform.Service.Battery.UUID, SERVICE_IDS.battery);
      return;
    }

    this.ensureService(this.platform.Service.Battery, `${this.device!.name} Battery`, SERVICE_IDS.battery);
  }

  private configureOptionalService(capability: string, serviceCtor: ServiceConstructor, subtype: string): void {
    if (!supportsCapability(this.device, capability)) {
      this.removeServiceIfPresent(serviceCtor.UUID, subtype);
      return;
    }

    this.ensureService(serviceCtor, this.device!.name, subtype);
  }

  private ensureService(serviceCtor: ServiceConstructor, displayName: string, subtype: string): Service {
    const existing = this.accessory.services.find(candidate =>
      candidate.UUID === serviceCtor.UUID && candidate.subtype === subtype,
    );

    return existing
      ?? this.accessory.addService(serviceCtor as unknown as typeof Service, displayName, subtype);
  }

  private removeStaleServices(): void {
    const desired = new Set<string>();

    if (supportsCapability(this.device, 'switch')) {
      desired.add(`${this.platform.Service.Switch.UUID}:${SERVICE_IDS.switch}`);
    }
    if (supportsCapability(this.device, 'temperature_sensor')) {
      desired.add(`${this.platform.Service.TemperatureSensor.UUID}:${SERVICE_IDS.temperature}`);
    }
    if (supportsCapability(this.device, 'humidity_sensor')) {
      desired.add(`${this.platform.Service.HumiditySensor.UUID}:${SERVICE_IDS.humidity}`);
    }
    if (supportsCapability(this.device, 'illuminance_sensor')) {
      desired.add(`${this.platform.Service.LightSensor.UUID}:${SERVICE_IDS.illuminance}`);
    }
    if (supportsCapability(this.device, 'occupancy_sensor')) {
      desired.add(`${this.platform.Service.OccupancySensor.UUID}:${SERVICE_IDS.occupancy}`);
    }
    if (supportsCapability(this.device, 'ias_zone_sensor')) {
      desired.add(`${this.platform.Service.ContactSensor.UUID}:${SERVICE_IDS.contact}`);
    }
    if (supportsBatteryService(this.device) && this.hasPrimaryService()) {
      desired.add(`${this.platform.Service.Battery.UUID}:${SERVICE_IDS.battery}`);
    }

    for (const service of this.accessory.services) {
      if (service.UUID === this.platform.Service.AccessoryInformation.UUID) {
        continue;
      }

      const key = `${service.UUID}:${service.subtype ?? ''}`;
      if (!desired.has(key)) {
        this.accessory.removeService(service);
      }
    }
  }

  private removeServiceIfPresent(uuid: string, subtype: string): void {
    const service = this.accessory.services.find(candidate => candidate.UUID === uuid && candidate.subtype === subtype);
    if (service) {
      this.accessory.removeService(service);
    }
  }

  private hasPrimaryService(): boolean {
    return supportsCapability(this.device, 'switch')
      || supportsCapability(this.device, 'temperature_sensor')
      || supportsCapability(this.device, 'humidity_sensor')
      || supportsCapability(this.device, 'illuminance_sensor')
      || supportsCapability(this.device, 'occupancy_sensor')
      || supportsCapability(this.device, 'ias_zone_sensor');
  }

  private getOnValue(): boolean {
    const raw = this.accessory.context.lastKnown?.state?.state?.value;
    if (typeof raw === 'boolean') {
      return raw;
    }
    if (typeof raw === 'string') {
      return raw.toUpperCase() === 'ON';
    }
    return false;
  }

  private async setOnValue(value: CharacteristicValue): Promise<void> {
    const { HAPStatus, HapStatusError } = this.platform.api.hap;

    if (!this.device || !supportsCapability(this.device, 'switch')) {
      throw new HapStatusError(HAPStatus.READ_ONLY_CHARACTERISTIC);
    }

    if (!this.gatewayAvailable) {
      throw new HapStatusError(HAPStatus.SERVICE_COMMUNICATION_FAILURE);
    }

    const next = Boolean(value);
    try {
      await this.platform.sendOnOffCommand(this.device.device_id, next);
    } catch (error) {
      this.platform.log.warn(
        `No se pudo cambiar ${this.device.name} a ${next ? 'ON' : 'OFF'}: ${(error as Error).message}`,
      );
      throw new HapStatusError(HAPStatus.SERVICE_COMMUNICATION_FAILURE);
    }

    this.applyEventChanges({
      state: {
        value: next ? 'ON' : 'OFF',
        quality: 'valid',
        ts: Math.floor(Date.now() / 1000),
      },
    });
  }

  private applyMeta(meta: DeviceMetaState): void {
    this.updateStatusFaults(meta.reachable ?? false);
  }

  private updateStatusFaults(reachableOverride?: boolean): void {
    const lastReachable = reachableOverride ?? this.accessory.context.lastKnown?.meta?.reachable ?? false;
    const healthy = this.gatewayAvailable && lastReachable;
    const faultValue = healthy
      ? this.platform.Characteristic.StatusFault.NO_FAULT
      : this.platform.Characteristic.StatusFault.GENERAL_FAULT;

    for (const service of this.accessory.services) {
      if (service.testCharacteristic(this.platform.Characteristic.StatusFault)) {
        service.updateCharacteristic(this.platform.Characteristic.StatusFault, faultValue);
      }
    }
  }

  private applyState(state: Record<string, AttributeValue>): void {
    const switchService = this.accessory.getServiceById(this.platform.Service.Switch, SERVICE_IDS.switch);
    if (switchService && state.state) {
      switchService.updateCharacteristic(this.platform.Characteristic.On, this.getOnValue());
    }

    const temperatureService = this.accessory.getServiceById(this.platform.Service.TemperatureSensor, SERVICE_IDS.temperature);
    if (temperatureService && typeof state.temperature?.value === 'number') {
      temperatureService.updateCharacteristic(
        this.platform.Characteristic.CurrentTemperature,
        clampNumber(state.temperature.value, -100, 100),
      );
    }

    const humidityService = this.accessory.getServiceById(this.platform.Service.HumiditySensor, SERVICE_IDS.humidity);
    if (humidityService && typeof state.humidity?.value === 'number') {
      humidityService.updateCharacteristic(
        this.platform.Characteristic.CurrentRelativeHumidity,
        clampNumber(state.humidity.value, 0, 100),
      );
    }

    const lightService = this.accessory.getServiceById(this.platform.Service.LightSensor, SERVICE_IDS.illuminance);
    if (lightService && typeof state.illuminance?.value === 'number') {
      lightService.updateCharacteristic(
        this.platform.Characteristic.CurrentAmbientLightLevel,
        clampNumber(state.illuminance.value, 0.0001, 100000),
      );
    }

    const occupancyService = this.accessory.getServiceById(this.platform.Service.OccupancySensor, SERVICE_IDS.occupancy);
    if (occupancyService && state.occupancy) {
      occupancyService.updateCharacteristic(
        this.platform.Characteristic.OccupancyDetected,
        this.toBoolean(state.occupancy)
          ? this.platform.Characteristic.OccupancyDetected.OCCUPANCY_DETECTED
          : this.platform.Characteristic.OccupancyDetected.OCCUPANCY_NOT_DETECTED,
      );
    }

    const contactService = this.accessory.getServiceById(this.platform.Service.ContactSensor, SERVICE_IDS.contact);
    if (contactService && state.contact) {
      contactService.updateCharacteristic(
        this.platform.Characteristic.ContactSensorState,
        this.toBoolean(state.contact)
          ? this.platform.Characteristic.ContactSensorState.CONTACT_DETECTED
          : this.platform.Characteristic.ContactSensorState.CONTACT_NOT_DETECTED,
      );
    }

    const batteryService = this.accessory.getServiceById(this.platform.Service.Battery, SERVICE_IDS.battery);
    if (batteryService && typeof state.battery?.value === 'number') {
      const batteryLevel = clampNumber(state.battery.value, 0, 100);
      batteryService.updateCharacteristic(this.platform.Characteristic.BatteryLevel, batteryLevel);
      batteryService.updateCharacteristic(
        this.platform.Characteristic.StatusLowBattery,
        batteryLevel <= 20
          ? this.platform.Characteristic.StatusLowBattery.BATTERY_LEVEL_LOW
          : this.platform.Characteristic.StatusLowBattery.BATTERY_LEVEL_NORMAL,
      );
      batteryService.updateCharacteristic(
        this.platform.Characteristic.ChargingState,
        this.platform.Characteristic.ChargingState.NOT_CHARGEABLE,
      );
    }
  }

  private toBoolean(attribute: AttributeValue): boolean {
    if (typeof attribute.value === 'boolean') {
      return attribute.value;
    }
    if (typeof attribute.value === 'string') {
      return ['on', 'true', '1'].includes(attribute.value.toLowerCase());
    }
    if (typeof attribute.value === 'number') {
      return attribute.value !== 0;
    }
    return false;
  }

  private ensureLastKnown(deviceId: string): void {
    if (!this.accessory.context.lastKnown || deviceKey(this.accessory.context.lastKnown.device_id) !== deviceKey(deviceId)) {
      this.accessory.context.lastKnown = normalizeStateSnapshot({
        device_id: deviceId,
        meta: {},
        state: {},
      });
    }
  }
}
