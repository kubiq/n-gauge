// ============================================================================
// log_priv.h  —  what the log_*.cpp files share, and nothing outside them sees.
// The logger's public face is logger.h. Inside it:
//
//   logger.cpp    the task and its SD lock, mounting, suspend/resume, closing,
//                 the sleep log, and what the writers share (idle, gear)
//   log_sink.cpp  LogFile: a file, its batch in PSRAM, and its encryption
//   log_names.cpp the session id, and dated or numbered file names
//   log_csv.cpp   the drive CSV, as a table of columns
//   log_gpx.cpp   the GPX trip track, and the trip distance
//   log_vbo.cpp   the .vbo track session
//
// Every writer runs on the logger task or under the SD lock (logger.h), so
// none of this needs locking of its own.
// ============================================================================
#pragma once
#include <Arduino.h>
#include "FS.h"
#include "logcrypt.h"
#include <stdarg.h>
#include <time.h>

// ---- LogFile (log_sink.cpp) -------------------------------------------------
// One log file and the batch its writes collect in. Batching is what keeps the
// card cheap: a write is one multi-sector transfer every second or two, not
// one per row - and, encrypted, one sealed record per batch, not per row.
//
// With a key set (settings.logKey) the file is logcrypt.h records: a segment
// header when it is opened, then each batch sealed as it goes out. Without
// one, plain text as it always was.
struct LogFile {
  File        f;
  char        path[48] = "";    // fits "/drive_2026-09-25_14-32_9_3fa81c.csv.nge"
  bool        open = false;     // the file is open and taking writes
  char       *buf = nullptr;    // the batch, in PSRAM - see logfile_alloc()
  size_t      cap = 0;
  size_t      len = 0;
  bool        enc = false;      // this segment is encrypted
  LogcryptSeg seg;
  // Runs after each batch goes out, before any flush (the GPX's closing tags).
  void      (*afterDrain)(LogFile &) = nullptr;
};

// The batch, in PSRAM. False without it - and then nothing may be logged.
bool logfile_alloc(LogFile &lf, size_t cap);
// Just opened (or reopened): a new segment with a fresh key, if encrypting.
void logfile_begin(LogFile &lf);
// Into the batch. When it does not fit, the batch goes out and it is tried
// again; false only for something longer than the whole batch (dropped).
bool logfile_printf(LogFile &lf, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
bool logfile_vprintf(LogFile &lf, const char *fmt, va_list ap);
bool logfile_puts(LogFile &lf, const char *s);
// The batch to the card - written, not yet flushed.
void logfile_drain(LogFile &lf);
// The batch to the card, flushed: what is written survives a power cut.
void logfile_commit(LogFile &lf);
// Closed: the segment ends (its key is wiped).
void logfile_end(LogFile &lf);
// Each logger pass: stir the entropy pool the segment keys come from.
void logsink_stir();

// ---- names (log_names.cpp) --------------------------------------------------
// A new session id, once per boot or wake: every file it writes carries it.
void lognames_new_session();
enum LogName { LOG_NAME_NONE, LOG_NAME_DATED, LOG_NAME_NUMBERED };
// The name a new log gets: "/<prefix><start time>_<session><ext>" while the
// clock is trusted, else "/<prefix><number>_<session><ext>". `base` is the
// plain extension (".csv"); the encrypted suffix is added when a key is set.
// `nextN` caches the next number, in RTC memory (one per kind of log).
LogName lognames_new(const char *prefix, const char *base, int *nextN,
                     char *out, size_t n);
// The dated name only, for the moment `t`, with the full extension given.
bool lognames_dated(const char *prefix, const char *ext, time_t t,
                    char *out, size_t n);

// ---- shared by the writers (logger.cpp) -------------------------------------
// The card cannot be written - an open failed for good, say. Every writer
// stops, as when there is no card at all.
void log_card_give_up();
// Parked a while (with a GPS fix): the writers thin out.
bool log_idle();
// Gear from rpm / OBD speed (calc_gear): 1..8, 0 = none (shifting, clutch
// slip, neutral, crawling), -1 = rpm or speed not current.
int  log_gear();

// ---- the writers, called by the logger task ---------------------------------
bool csvlog_init();                 // its batch; false = no logging
void csvlog_task();                 // a row at LOG_INTERVAL_MS, opening the file
void csvlog_close();
void csvlog_note_pass_us(uint32_t us);   // the logger pass, for logMaxMs
const char *csvlog_path();

bool gpxlog_init();
void gpxlog_task();                 // per GNSS fix; also counts the trip
void gpxlog_close();
void gpxlog_card_back();            // the card is back: an open may be tried again
const char *gpxlog_path();

bool vbolog_init();
void vbolog_task();                 // per GNSS fix, in a track session
void vbolog_close();
const char *vbolog_path();
