#pragma once

// Boot/wake phase timing instrumentation. Compiled in only when the build sets
// -DBOOT_PROFILE=1 (see platformio.local.ini [env:profile]); otherwise every
// mark compiles to nothing.
//
// Output format (cumulative ms since boot, delta since previous mark):
//   [INF][BOOT] +  1234 ms (+ 567) sd mount
//
// The e-ink share of each phase comes from the SDK's own "Refresh done (N ms)"
// logs. Methodology + previous measured budgets: docs/boot-sleep-optimization.md
//
// Marks are called from both the main task and the render task. The lastMark
// static is unsynchronized; the call sites never actually overlap (the main
// task blocks or idles while the render task paints), and this is a
// profiling-only build, so a spinlock isn't worth the noise.

#if BOOT_PROFILE
#include <Arduino.h>
#include <Logging.h>

inline void bootMark(const char* label) {
  static unsigned long lastMark = 0;
  const unsigned long now = millis();
  LOG_INF("BOOT", "+%6lu ms (+%5lu) %s", now, now - lastMark, label);
  lastMark = now;
}
#define BOOT_MARK(label) bootMark(label)
#else
#define BOOT_MARK(label) ((void)0)
#endif
