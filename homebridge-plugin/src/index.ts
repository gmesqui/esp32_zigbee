import type { API } from 'homebridge';

import { Esp32ZigbeePlatform } from './platform';
import { PLATFORM_NAME, PLUGIN_NAME } from './model';

export = (api: API): void => {
  api.registerPlatform(PLUGIN_NAME, PLATFORM_NAME, Esp32ZigbeePlatform);
};
