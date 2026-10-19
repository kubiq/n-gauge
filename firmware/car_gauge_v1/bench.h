// ============================================================================
// bench.h  —  the desk tools: the serial console (BENCH_CONSOLE), the OBD
// rate print (FRAME_STATS) and the wake-on-motion test (WOM_BENCH) - see
// config.h. All 0 for the car, and then every call here is an empty inline,
// so loop() calls them without an #if.
// ============================================================================
#pragma once
#include <stdint.h>
#include "config.h"

#if BENCH_CONSOLE || FRAME_STATS || WOM_BENCH
// Once per loop pass, first: reads a console command, prints the stats.
void bench_task(uint32_t now);
#else
static inline void bench_task(uint32_t) {}
#endif

#if BENCH_CONSOLE
// 's': synthetic input - the g-meter circling, boost sweeping 0-1.9 bar - so
// frame rates under constant change can be measured with no car.
bool bench_synth();
// 'r': the notification rim hidden, as in the car, where nothing is
// normally faulted.
bool bench_no_rim();
// 'v<kmh>': the speed the AP-while-driving logic sees. -1 = the real GNSS,
// 0 = no fix. Overrides what THAT logic sees, not the gps fields themselves:
// the GNSS task rewrites gps.haveFix on every poll, module or not, so a value
// poked into it lasts about 5 ms.
int  bench_ap_kmh();
#else
static inline bool bench_synth()  { return false; }
static inline bool bench_no_rim() { return false; }
static inline int  bench_ap_kmh() { return -1; }
#endif
