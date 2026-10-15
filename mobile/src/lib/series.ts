// A drive's readings over time, for its charts: the CSV (a row every
// 500 ms) cut down to at most SERIES_POINTS buckets. Worked out once and
// cached with the summaries (trip-cache.ts), so opening a drive never reads
// or decrypts its CSV again. Pure, so it is tested with node.

export const SERIES_POINTS = 600;

export type Series = {
  /** Seconds from the first row to the last. */
  durationS: number;
  /**
   * Epoch ms of the first row, from the `utc` column - so a moment on the
   * chart can be found on the map (the GPX's clock). Null when the gauge's
   * clock was not set during the drive.
   */
  startUtc: number | null;
  /** Each bucket's middle, seconds from the start. */
  t: number[];
  /** Per bucket; null where the bucket has no reading (a dropout, no column). */
  kmh: (number | null)[];
  boostBar: (number | null)[];
  oilC: (number | null)[];
  coolantC: (number | null)[];
  volts: (number | null)[];
};

type Keep = 'max' | 'mean';
// Speed and boost keep the bucket's peak - a 2 s spike of boost is the point
// of the chart; temperatures and voltage its mean.
const CHANNELS: { key: Exclude<keyof Series, 'durationS' | 'startUtc' | 't'>; cols: string[]; keep: Keep }[] = [
  // The car's speed; GPS speed for logs from before obdKmh existed.
  { key: 'kmh', cols: ['obdKmh', 'speedKmh'], keep: 'max' },
  // The true value, vacuum included: the gauge stops at 0.00, the chart need not.
  { key: 'boostBar', cols: ['boostBar'], keep: 'max' },
  { key: 'oilC', cols: ['oilC'], keep: 'mean' },
  { key: 'coolantC', cols: ['coolantC'], keep: 'mean' },
  { key: 'volts', cols: ['volts'], keep: 'mean' },
];

const round = (v: number) => Math.round(v * 100) / 100;

export function csvSeries(text: string, points = SERIES_POINTS): Series {
  // CRLF, as the gauge writes them: split on \n alone left "\r" in the last
  // column, and Number("\r") is 0 - an empty voltage read as 0 V.
  const lines = text.split(/\r?\n/);
  const head = (lines[0] ?? '').trim().split(',');
  const iMs = head.indexOf('ms');
  const empty: Series = { durationS: 0, startUtc: null, t: [], kmh: [], boostBar: [], oilC: [], coolantC: [], volts: [] };
  if (iMs < 0) return empty;
  // A channel's column: the first of its names the header has, with data.
  const rows = lines.slice(1).filter(Boolean).map((l) => l.split(','));
  const colOf = CHANNELS.map((ch) => {
    for (const name of ch.cols) {
      const i = head.indexOf(name);
      if (i >= 0 && rows.some((f) => f[i] !== undefined && f[i] !== '')) return i;
    }
    return -1;
  });

  // Uptime ms; a restart mid-log makes it jump back - those rows are left out.
  const first = rows.length ? Number(rows[0][iMs]) : NaN;
  let last = first;
  for (const f of rows) {
    const ms = Number(f[iMs]);
    if (ms >= first && ms > last) last = ms;
  }
  if (!Number.isFinite(first) || last <= first) return empty;
  const span = last - first;
  const n = Math.min(points, rows.length);

  const sum = CHANNELS.map(() => new Array<number>(n).fill(0));
  const cnt = CHANNELS.map(() => new Array<number>(n).fill(0));
  const peak = CHANNELS.map(() => new Array<number>(n).fill(-Infinity));
  for (const f of rows) {
    const ms = Number(f[iMs]);
    if (!(ms >= first)) continue;
    const b = Math.min(n - 1, Math.floor(((ms - first) / span) * n));
    CHANNELS.forEach((_, c) => {
      const i = colOf[c];
      if (i < 0 || f[i] === undefined || f[i] === '') return;
      const v = Number(f[i]);
      if (!Number.isFinite(v)) return;
      sum[c][b] += v;
      cnt[c][b]++;
      if (v > peak[c][b]) peak[c][b] = v;
    });
  }

  // The clock at the first row. `utc` is whole seconds, `ms` the uptime to
  // the millisecond: utc - ms on each row is the uptime's start, a little
  // early by the truncated fraction - the largest of them is the closest.
  const iUtc = head.indexOf('utc');
  let zero = -Infinity;
  if (iUtc >= 0) {
    for (const f of rows) {
      const ms = Number(f[iMs]);
      const utc = f[iUtc] ? Date.parse(f[iUtc]) : NaN;
      if (ms >= first && Number.isFinite(utc)) zero = Math.max(zero, utc - ms);
    }
  }
  const out: Series = { ...empty, durationS: span / 1000, startUtc: Number.isFinite(zero) ? zero + first : null };
  for (let b = 0; b < n; b++) out.t.push(round(((b + 0.5) / n) * (span / 1000)));
  CHANNELS.forEach((ch, c) => {
    out[ch.key] = sum[c].map((s, b) =>
      cnt[c][b] ? round(ch.keep === 'max' ? peak[c][b] : s / cnt[c][b]) : null,
    );
  });
  return out;
}
