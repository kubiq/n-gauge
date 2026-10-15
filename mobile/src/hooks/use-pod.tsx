// The one connection to the pod, shared by every screen: its WiFi
// credentials and whether the app is on its network right now.

import { createContext, type ReactNode, useCallback, useContext, useEffect, useState } from 'react';

import PodWifi from '@modules/pod-wifi';
import { t } from '@/i18n';
import { loadCreds, saveCreds } from '@/lib/store';
import type { WifiCreds } from '@/lib/wifi-qr';

// Long enough to find the pod and for the user to tap the system prompt.
const CONNECT_TIMEOUT_MS = 30_000;

type State = 'idle' | 'connecting' | 'connected';

type Pod = {
  creds: WifiCreds | null;
  /** False until the stored credentials have been read. */
  ready: boolean;
  state: State;
  error: string | null;
  setCreds: (c: WifiCreds) => Promise<void>;
  connect: () => Promise<boolean>;
  disconnect: () => void;
};

const PodContext = createContext<Pod | null>(null);

export function PodProvider({ children }: { children: ReactNode }) {
  const [creds, setCredsState] = useState<WifiCreds | null>(null);
  const [ready, setReady] = useState(false);
  const [state, setState] = useState<State>('idle');
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    loadCreds()
      .then(setCredsState)
      .finally(() => setReady(true));
    const sub = PodWifi.addListener('onLost', () => {
      setState('idle');
      setError(t('Lost the gauge’s WiFi'));
    });
    return () => sub.remove();
  }, []);

  const setCreds = useCallback(async (c: WifiCreds) => {
    await saveCreds(c);
    setCredsState(c);
  }, []);

  const connect = useCallback(async () => {
    if (!creds) return false;
    setError(null);
    setState('connecting');
    try {
      await PodWifi.connect(creds.ssid, creds.pass, CONNECT_TIMEOUT_MS);
      setState('connected');
      return true;
    } catch (e) {
      setState('idle');
      setError(e instanceof Error ? e.message : String(e));
      return false;
    }
  }, [creds]);

  const disconnect = useCallback(() => {
    PodWifi.disconnect();
    setState('idle');
  }, []);

  return (
    <PodContext.Provider value={{ creds, ready, state, error, setCreds, connect, disconnect }}>
      {children}
    </PodContext.Provider>
  );
}

export function usePod(): Pod {
  const p = useContext(PodContext);
  if (!p) throw new Error('usePod outside PodProvider');
  return p;
}
