// A GPX as other apps should get it: valid XML, one closing tail.
//
// The pod keeps its GPX valid on the card by writing the closing tags after
// every batch and seeking back over them for the next one (gpxWriteTail() in
// log_gpx.cpp). A copy the phone pulled while the file was still being
// written, and then continued, can therefore hold that old tail in the
// middle, with a trackpoint cut short after it. Pulls now continue over the
// tail (store.ts), but copies made before that may still be broken - so the
// shared file is rebuilt from the lines that are whole.

/** log_gpx.cpp's GPX_TAIL. */
export const GPX_TAIL = '</trkseg></trk></gpx>\n';

const TRKPT = /^<trkpt .*<\/trkpt>$/;
const SEGMENT_BREAK = '</trkseg><trkseg>';

export function cleanGpx(text: string): string {
  const start = text.indexOf('<trkseg>');
  if (start < 0) return text;   // not one of the pod's tracks: leave it be
  const head = text.slice(0, start + '<trkseg>'.length);
  const out: string[] = [];
  let points = 0;
  for (const line of text.slice(head.length).split('\n')) {
    if (TRKPT.test(line)) {
      out.push(line);
      points++;
    } else if (line === SEGMENT_BREAK && points && out[out.length - 1] !== SEGMENT_BREAK) {
      // A break with points before it; a run of them, or one at the start,
      // would only make empty segments.
      out.push(line);
    }
  }
  if (out[out.length - 1] === SEGMENT_BREAK) out.pop();
  return `${head}\n${out.length ? `${out.join('\n')}\n` : ''}${GPX_TAIL}`;
}
