import { useMemo, useState } from 'react';
import { ActivityIndicator, StyleSheet, Text, View } from 'react-native';
import { WebView } from 'react-native-webview';

import { Note } from '@/components/ui';
import { t } from '@/i18n';
import type { TrackPoint } from '@/lib/trips';
import { type Colors, themed, useColors } from '@/theme';

// MapLibre GL JS in a WebView, on OpenFreeMap's vector tiles: no API key, no
// native map SDK to build. Both come over the internet, so the map only
// loads off the pod's WiFi (while connected, the app is bound to the pod).
// 5.x, not 6: 6 no longer ships dist/maplibre-gl.js (404), the plain script
// build a page without a bundler needs.
const MAPLIBRE = 'https://cdn.jsdelivr.net/npm/maplibre-gl@5.24.0/dist';
// The theme picks the style: 'dark', or 'positron' for the light theme.
const STYLES = 'https://tiles.openfreemap.org/styles/';

// Speed bands of this width become one line each, so the colour follows the
// speed without a feature per point.
const BAND_KMH = 10;

/** The track as GeoJSON lines, one per run of points in the same speed band. */
export function speedLines(track: TrackPoint[]) {
  const features = [];
  let run: [number, number][] = [];
  let band = -1;
  for (const [lon, lat, kmh] of track) {
    const b = Math.floor(kmh / BAND_KMH);
    if (b !== band && run.length) {
      run.push([lon, lat]);   // share the point, so the line has no gaps
      features.push({ type: 'Feature', properties: { kmh: band * BAND_KMH + BAND_KMH / 2 }, geometry: { type: 'LineString', coordinates: run } });
      run = [];
    }
    band = b;
    run.push([lon, lat]);
  }
  if (run.length > 1) {
    features.push({ type: 'Feature', properties: { kmh: band * BAND_KMH + BAND_KMH / 2 }, geometry: { type: 'LineString', coordinates: run } });
  }
  return { type: 'FeatureCollection', features };
}

function html(track: TrackPoint[], bbox: [number, number, number, number], C: Colors) {
  const lines = JSON.stringify(speedLines(track));
  const ends = JSON.stringify({
    type: 'FeatureCollection',
    features: [track[0], track[track.length - 1]].map((p, i) => ({
      type: 'Feature',
      properties: { end: i },
      geometry: { type: 'Point', coordinates: [p[0], p[1]] },
    })),
  });
  return `<!DOCTYPE html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<link rel="stylesheet" href="${MAPLIBRE}/maplibre-gl.css">
<script src="${MAPLIBRE}/maplibre-gl.js"></script>
<style>html,body,#m{margin:0;height:100%;background:${C.bg}}</style></head><body><div id="m"></div><script>
const say = (m) => window.ReactNativeWebView.postMessage(m);
window.onerror = (msg) => say('error:' + msg);
let loaded = false;
if (!window.maplibregl) say('error:the map library did not load');
else {
  const map = new maplibregl.Map({
    // The credit is shown under the map by the app instead (see TrackMap):
    // MapLibre's own box opened as a white bar across the bottom of it.
    container: 'm', style: '${STYLES}${C.mapStyle}', attributionControl: false,
    bounds: ${JSON.stringify(bbox)}, fitBoundsOptions: { padding: 32 },
  });
  // Only before the map is up: a single missing tile later is not worth a warning.
  map.on('error', (e) => { if (!loaded) say('error:' + ((e.error && e.error.message) || 'map error')); });
  map.on('load', () => {
    map.addSource('t', { type: 'geojson', data: ${lines} });
    map.addLayer({ id: 'casing', type: 'line', source: 't',
      layout: { 'line-join': 'round', 'line-cap': 'round' },
      paint: { 'line-color': '#000', 'line-width': 7, 'line-opacity': 0.5 } });
    map.addLayer({ id: 'speed', type: 'line', source: 't',
      layout: { 'line-join': 'round', 'line-cap': 'round' },
      paint: { 'line-width': 4, 'line-color': ['interpolate', ['linear'], ['get', 'kmh'],
        0, '#3b82f6', 50, '#22c55e', 90, '#eab308', 130, '#f97316', 170, '#ef4444'] } });
    map.addSource('e', { type: 'geojson', data: ${ends} });
    map.addLayer({ id: 'ends', type: 'circle', source: 'e',
      paint: { 'circle-radius': 6, 'circle-stroke-width': 2, 'circle-stroke-color': '#fff',
        'circle-color': ['match', ['get', 'end'], 0, '#22c55e', '#ef4444'] } });
    loaded = true;
    say('ok');
    // The first moment the tiles are drawn too: the spinner goes then.
    map.once('idle', () => say('drawn'));
  });
}
</script></body></html>`;
}

/** The map's box with a spinner: while the track is read, before TrackMap. */
export function MapLoading() {
  const styles = useStyles();
  return (
    <View style={styles.box}>
      <Spinner />
    </View>
  );
}

function Spinner() {
  const C = useColors();
  const styles = useStyles();
  return (
    <View style={styles.spinner} pointerEvents="none">
      <ActivityIndicator size="large" color={C.accent} />
    </View>
  );
}

export function TrackMap({ track, bbox }: { track: TrackPoint[]; bbox: [number, number, number, number] }) {
  const [problem, setProblem] = useState<string | null>(null);
  const [drawn, setDrawn] = useState(false);
  const C = useColors();
  const styles = useStyles();
  const source = useMemo(() => ({ html: html(track, bbox, C), baseUrl: 'https://n-gauge.local/' }), [track, bbox, C]);
  return (
    <View style={{ gap: 8 }}>
      <View style={styles.box}>
        <WebView
          source={source}
          originWhitelist={['*']}
          style={{ backgroundColor: C.bg }}
          onMessage={(e) => {
            const m = e.nativeEvent.data;
            if (m === 'drawn') setDrawn(true);
            else if (m === 'ok') setProblem(null);
            // The real reason, plus the usual one: it is easy to forget that
            // a connection to the pod takes the app's internet with it.
            else setProblem(t('The map did not load ({reason}). It needs internet - not while connected to the gauge.', { reason: m.replace(/^error:/, '') }));
          }}
          onError={(e) =>
            setProblem(t('The map did not load ({reason}). It needs internet - not while connected to the gauge.', { reason: e.nativeEvent.description }))
          }
        />
        {!drawn && !problem ? <Spinner /> : null}
      </View>
      {problem ? <Note tone="bad">{problem}</Note> : null}
      <View style={styles.legend}>
        {[
          ['#3b82f6', '0'],
          ['#22c55e', '50'],
          ['#eab308', '90'],
          ['#f97316', '130'],
          ['#ef4444', '170+ km/h'],
        ].map(([c, t]) => (
          <View key={t} style={styles.legendItem}>
            <View style={[styles.swatch, { backgroundColor: c }]} />
            <Note>{t}</Note>
          </View>
        ))}
      </View>
      {/* Required by the licences (ODbL for the data); the same credit
          MapLibre's control showed, in the app's own style. */}
      <Text style={styles.credit}>© OpenStreetMap contributors · OpenMapTiles · OpenFreeMap</Text>
    </View>
  );
}

const useStyles = themed((C) =>
  StyleSheet.create({
    box: { height: 380, borderRadius: 12, overflow: 'hidden', borderColor: C.border, borderWidth: 1, backgroundColor: C.bg },
    spinner: { position: 'absolute', top: 0, right: 0, bottom: 0, left: 0, alignItems: 'center', justifyContent: 'center', backgroundColor: C.bg },
    legend: { flexDirection: 'row', flexWrap: 'wrap', gap: 12 },
    legendItem: { flexDirection: 'row', alignItems: 'center', gap: 4 },
    swatch: { width: 14, height: 4, borderRadius: 2 },
    credit: { color: C.dim, fontSize: 11 },
  }),
);
