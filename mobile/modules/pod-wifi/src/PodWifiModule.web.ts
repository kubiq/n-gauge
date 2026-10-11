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
}

export default registerWebModule(PodWifiModule, 'PodWifiModule');
