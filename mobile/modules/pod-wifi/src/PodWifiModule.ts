import { NativeModule, requireNativeModule } from 'expo';

import type { PodWifiEvents } from './PodWifi.types';

declare class PodWifiModule extends NativeModule<PodWifiEvents> {
  /** Shows Android's "connect to this device" prompt; resolves once joined. */
  connect(ssid: string, passphrase: string, timeoutMs: number): Promise<void>;
  /** Leaves the pod's network and gives the app its internet back. */
  disconnect(): void;
  isConnected(): boolean;
}

export default requireNativeModule<PodWifiModule>('PodWifi');
