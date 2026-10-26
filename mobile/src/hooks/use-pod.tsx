// The one connection to the pod, shared by every screen: its WiFi
// credentials and whether the app is on its network right now.

import { useFocusEffect } from 'expo-router';
import { createContext, type ReactNode, useCallback, useContext, useEffect, useState } from 'react';
import { Linking, ToastAndroid } from 'react-native';

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
  /** `asked` false: a connect nobody tapped for - no toast, no WiFi panel. */
  connect: (asked?: boolean) => Promise<boolean>;
  disconnect: () => void;
  /** Counts connections: a new one, a new number. */
  session: number;
  /** How many open screens are using the connection (useHoldPod). */
  holders: number;
  /** Marks the connection in use; returns the release. */
  hold: () => () => void;
};

const PodContext = createContext<Pod | null>(null);

export function PodProvider({ children }: { children: ReactNode }) {
  const [creds, setCredsState] = useState<WifiCreds | null>(null);
  const [ready, setReady] = useState(false);
  const [state, setState] = useState<State>('idle');
  const [error, setError] = useState<string | null>(null);
  const [session, setSession] = useState(0);

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

  // A failed connect says why (2026-10-26: with the phone's WiFi off, a tap
  // on the gauge waited 30 s and went back to "Not connected", no word of
  // why): the reason short in `error` - the header chip shows it - and, when
  // asked for, a toast; WiFi off also opens Android's WiFi panel, one tap to
  // switch it on. Not asked for (the sync on opening the app): quiet.
  const connect = useCallback(
    async (asked = true) => {
      if (!creds) return false;
      setError(null);
      setState('connecting');
      try {
        await PodWifi.connect(creds.ssid, creds.pass, CONNECT_TIMEOUT_MS);
        setSession((n) => n + 1);
        setState('connected');
        return true;
      } catch (e) {
        setState('idle');
        const code = (e as { code?: string }).code;
        if (code === 'ERR_WIFI_OFF') {
          setError(t('WiFi is off'));
          if (asked) {
            ToastAndroid.show(t('WiFi is off - turn it on to reach the gauge.'), ToastAndroid.LONG);
            Linking.sendIntent('android.settings.panel.action.WIFI').catch(() => {});
          }
        } else if (code === 'ERR_UNAVAILABLE') {
          setError(t('Gauge not found'));
          if (asked) {
            ToastAndroid.show(
              t('Gauge not found, or the prompt was declined. It must be awake: car on, or a door opened.'),
              ToastAndroid.LONG,
            );
          }
        } else {
          const msg = e instanceof Error ? e.message : String(e);
          setError(msg);
          if (asked) ToastAndroid.show(t('Could not connect: {error}', { error: msg }), ToastAndroid.LONG);
        }
        return false;
      }
    },
    [creds],
  );

  const disconnect = useCallback(() => {
    PodWifi.disconnect();
    setState('idle');
  }, []);

  // Screens that use the connection while open - the live readings, the
  // gauge's settings, the log key - so "disconnect after syncing" waits for
  // them (use-sync.tsx). A count, not a flag: one can open over another.
  const [holders, setHolders] = useState(0);
  const hold = useCallback(() => {
    setHolders((n) => n + 1);
    let released = false;
    return () => {
      if (released) return;
      released = true;
      setHolders((n) => n - 1);
    };
  }, []);

  return (
    <PodContext.Provider value={{ creds, ready, state, error, setCreds, connect, disconnect, session, holders, hold }}>
      {children}
    </PodContext.Provider>
  );
}

export function usePod(): Pod {
  const p = useContext(PodContext);
  if (!p) throw new Error('usePod outside PodProvider');
  return p;
}

/** This screen uses the connection while it is the one shown. */
export function useHoldPod(): void {
  const { hold } = usePod();
  useFocusEffect(useCallback(() => hold(), [hold]));
}
