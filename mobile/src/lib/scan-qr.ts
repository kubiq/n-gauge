// Reads the pod's WiFi QR with Google's code scanner (Play services) where
// there is one. The pod draws a ~12 mm code, and a plain camera view could
// not read it: too small at a distance the lens can focus at, and no zoom.
// Google's scanner has its own zoom (and auto-zoom), and needs no camera
// permission - the scanning UI runs in Play services, not in this app.

import { CameraView } from 'expo-camera';

import { parseWifiQr, type WifiCreds } from './wifi-qr';

export type ScanOutcome =
  | { kind: 'ok'; creds: WifiCreds }
  | { kind: 'wrong'; data: string }
  | { kind: 'cancelled' }
  /** No Play services scanner: use the in-app camera screen instead. */
  | { kind: 'unavailable' };

/**
 * The text actually in the code. `data` is ML Kit's displayValue, which for a
 * code it recognises as WiFi is a summary rather than the `WIFI:...` string -
 * so the pod's code was rejected as "not the pod's" whenever ML Kit typed it.
 */
export function qrText(e: { data: string; raw?: string }): string {
  return e.raw ?? e.data;
}

export async function scanPodQr(): Promise<ScanOutcome> {
  if (!CameraView.isModernBarcodeScannerAvailable) return { kind: 'unavailable' };
  let data: string | null = null;
  const sub = CameraView.onModernBarcodeScanned((e) => {
    data = qrText(e);
  });
  try {
    // Resolves after the event has been sent (see CameraViewModule.kt).
    await CameraView.launchScanner({ barcodeTypes: ['qr'], isPinchToZoomEnabled: true });
  } catch {
    return { kind: 'cancelled' };
  } finally {
    sub.remove();
  }
  if (data === null) return { kind: 'cancelled' };
  const creds = parseWifiQr(data);
  return creds ? { kind: 'ok', creds } : { kind: 'wrong', data };
}
