// The pod's WiFi screen shows a QR of `WIFI:T:WPA;S:<ssid>;P:<pass>;;` (built
// in ui.cpp from settings.apSsid / apPass). Scanning it is how the app learns
// the passphrase, which is random per pod and never leaves it otherwise.

export type WifiCreds = { ssid: string; pass: string };

// The format escapes \ ; , : and " with a backslash. The pod's own SSID and
// alphabet never need it, but a renamed AP could.
function splitFields(body: string): string[] {
  const out: string[] = [];
  let cur = '';
  for (let i = 0; i < body.length; i++) {
    const c = body[i];
    if (c === '\\' && i + 1 < body.length) {
      cur += body[++i];
    } else if (c === ';') {
      out.push(cur);
      cur = '';
    } else {
      cur += c;
    }
  }
  if (cur) out.push(cur);
  return out;
}

export function parseWifiQr(text: string): WifiCreds | null {
  if (!text.startsWith('WIFI:')) return null;
  let ssid: string | undefined;
  let pass = '';
  for (const f of splitFields(text.slice(5))) {
    const colon = f.indexOf(':');
    if (colon < 0) continue;
    const k = f.slice(0, colon);
    const v = f.slice(colon + 1);
    if (k === 'S') ssid = v;
    else if (k === 'P') pass = v;
  }
  if (!ssid) return null;
  return { ssid, pass };
}
