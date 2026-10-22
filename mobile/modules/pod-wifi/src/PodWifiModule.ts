import { NativeModule, requireNativeModule } from 'expo';

import type { PodWifiEvents } from './PodWifi.types';

declare class PodWifiModule extends NativeModule<PodWifiEvents> {
  /** Shows Android's "connect to this device" prompt; resolves once joined. */
  connect(ssid: string, passphrase: string, timeoutMs: number): Promise<void>;
  /** Leaves the pod's network and gives the app its internet back. */
  disconnect(): void;
  isConnected(): boolean;
  /** The fingerprint the gauge beacons (hex), if a scan in the last maxAgeMs heard it. */
  seenBeacon(maxAgeMs: number): string | null;
  /** The fingerprints of the drives the phone has, for the background check. */
  setKnownBeacons(fps: string[]): void;
  /** The background check, with the words of its note; or off. */
  watchBeacon(on: boolean, title: string, text: string): void;
}

export default requireNativeModule<PodWifiModule>('PodWifi');
