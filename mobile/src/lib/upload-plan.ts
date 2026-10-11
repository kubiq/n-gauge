// What to upload of one inbox file, given what the server already holds.
// Pure, so it can be tested without a server.
//
// The phone holds each file whole from byte 0 (store.ts), `uploaded` bytes
// of it recorded as on the server and `pending` after them not. The server's
// size normally equals `uploaded`, but not always: a PUT can succeed with
// its reply lost (the server is ahead), the server can have lost data (it is
// behind), or the pod's file was replaced (pull restarted it at 0 while the
// server still has the old, longer one). Holding the whole file, the phone
// can repair all three.

export type InboxEntry = { uploaded: number; pending: number };

export type UploadStep =
  /** Nothing waiting. */
  | { kind: 'none' }
  /** The server already has exactly what we hold; just record it. */
  | { kind: 'done'; size: number }
  /**
   * PUT the file from byte `offset` to its end. Offset 0 is the server's
   * "replace", for when it holds more than we do.
   */
  | { kind: 'send'; offset: number };

export function planUpload(e: InboxEntry, server: number | undefined): UploadStep {
  if (e.pending === 0) return { kind: 'none' };
  const s = server ?? 0;
  const end = e.uploaded + e.pending;
  if (s === end) return { kind: 'done', size: s };
  if (s < end) return { kind: 'send', offset: s };
  return { kind: 'send', offset: 0 };
}
