// ============================================================================
// fixring.h  —  every GNSS fix, queued for the logs: a ring one task (the
//               GNSS's) writes and several readers (the GPX, the .vbo) each
//               read at their own pace. Pure, so the host tests run it.
//
// 2026-10-26: the logs read only the latest fix, once per logger pass. A card
// that stalls a write - 0.6-1.35 s, a few times a drive - held the logger,
// the GNSS task kept replacing the one fix, and the GPX had a hole while
// moving of just that length (~30 fixes at 25 Hz, 10-28 holes a drive). Now
// every fix goes into the ring and a reader takes everything it has not had:
// after a stall, late, but all of them.
//
// One writer. `head` counts the fixes pushed; fix i lives in slot i % N. The
// writer fills the slot, then raises head - so while it writes fix h, head is
// still h. A reader copies its next fix, then reads head again: if head has
// reached next + N meanwhile, the writer has started over that slot and the
// copy may be torn, so it is thrown away. A reader more than N - 1 behind
// jumps to the oldest fix still whole and counts the rest as lost.
// ============================================================================
#pragma once
#include <stdint.h>

template <typename T, uint32_t N>
struct FixRing {
  T *slots;                   // N of them, the caller's (PSRAM on the board)
  volatile uint32_t head;     // fixes pushed so far
};

// One reader's place: the next fix it wants. Starts at the ring's head (only
// what comes after), set on its first read.
struct FixCursor {
  uint32_t next;
  bool     started;
};

template <typename T, uint32_t N>
static inline void fixring_push(FixRing<T, N> *r, const T &v) {
  const uint32_t h = r->head;
  r->slots[h % N] = v;
  __sync_synchronize();       // the slot is whole before head says so
  r->head = h + 1;
}

// The next fix this reader has not had, oldest first; false when it has had
// them all. `lost` grows by the fixes overwritten before it got to them.
template <typename T, uint32_t N>
static inline bool fixring_next(FixRing<T, N> *r, FixCursor *c, T *out, uint32_t *lost) {
  if (!c->started) {
    c->started = true;
    c->next = r->head;
  }
  for (;;) {
    const uint32_t h = r->head;
    __sync_synchronize();
    if (c->next == h) return false;
    if (h - c->next > N - 1) {          // fallen behind: what is left whole
      *lost += (h - (N - 1)) - c->next;
      c->next = h - (N - 1);
    }
    const T v = r->slots[c->next % N];
    __sync_synchronize();
    if (r->head - c->next >= N) continue;   // overwritten while copied: again
    *out = v;
    c->next++;
    return true;
  }
}
