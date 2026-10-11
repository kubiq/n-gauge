import { CameraView, useCameraPermissions } from 'expo-camera';
import { router } from 'expo-router';
import { useRef, useState } from 'react';
import { StyleSheet, View } from 'react-native';

import { Button, Note, Screen } from '@/components/ui';
import { usePod } from '@/hooks/use-pod';
import { qrText } from '@/lib/scan-qr';
import { parseWifiQr } from '@/lib/wifi-qr';
import { S } from '@/theme';

// The pod's WiFi screen (tap through the views on the gauge) shows the QR.
export default function Scan() {
  const [perm, requestPerm] = useCameraPermissions();
  const { setCreds } = usePod();
  const [wrong, setWrong] = useState(false);
  // onBarcodeScanned fires per frame; take the first good one only.
  const done = useRef(false);

  if (!perm) return null;
  if (!perm.granted) {
    return (
      <Screen>
        <Note>The camera reads the QR code on the pod’s WiFi screen. Nothing else is recorded.</Note>
        <Button title="Allow camera" onPress={requestPerm} />
      </Screen>
    );
  }

  return (
    <View style={styles.wrap}>
      <CameraView
        style={StyleSheet.absoluteFill}
        barcodeScannerSettings={{ barcodeTypes: ['qr'] }}
        onBarcodeScanned={(e) => {
          if (done.current) return;
          const c = parseWifiQr(qrText(e));
          if (!c) {
            setWrong(true);
            return;
          }
          done.current = true;
          setCreds(c).then(() => router.back());
        }}
      />
      <View style={styles.hint}>
        <Note tone={wrong ? 'bad' : undefined}>
          {wrong ? 'That is not the pod’s WiFi code.' : 'Point at the QR on the pod’s WiFi screen.'}
        </Note>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  wrap: { flex: 1 },
  hint: { position: 'absolute', bottom: S.lg, left: S.md, right: S.md, padding: S.md, backgroundColor: '#000a', borderRadius: 8 },
});
