import { NativeModule, registerWebModule } from 'expo';

import type { PodWifiEvents } from './PodWifi.types';

// A browser cannot join a WiFi network. On web the app assumes the computer is
// already on the pod's AP, which is enough for working on the screens.
class PodWifiModule extends NativeModule<PodWifiEvents> {
  async connect(_ssid: string, _passphrase: string, _timeoutMs: number): Promise<void> {}
  disconnect(): void {}
  isConnected(): boolean {
    return true;
  }
  // No WiFi scans in a browser: no beacon.
  seenBeacon(_maxAgeMs: number): string | null {
    return null;
  }
  setKnownBeacons(_fps: string[]): void {}
  watchBeacon(_on: boolean, _title: string, _text: string): void {}
}

export default registerWebModule(PodWifiModule, 'PodWifiModule');
