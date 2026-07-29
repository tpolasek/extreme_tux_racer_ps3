/* PS3 performance log implementation.
 *
 * Owns a single FILE* plus a fixed-size table of tag slots. Each
 * ps3_perf_end(tag) adds elapsed µs to that tag's accumulator and bumps
 * its call count; once per wall-clock window (PS3_PERF_WINDOW_US) every
 * active slot writes one CSV row and the accumulators reset.
 *
 * Slots are matched by the tag pointer (callers pass string literals), so
 * there's no per-call strcmp. The table is small and searched linearly —
 * fine for the handful of timers a real port needs.
 *
 * All functions early-return when g_logFile is NULL, so failed-open (or
 * not-opened-yet) is harmless.
 */
/* bh.h defines OS_PS3 from __PPU__ for the game sources; this TU doesn't
 * include bh.h, so mirror the same detection before including ps3_log.h. */
#ifdef __PPU__
#	ifndef OS_PS3
#		define OS_PS3
#	endif
#endif

#include "ps3_log.h"

#ifdef OS_PS3

#include <cstdio>
#include <cstring>
#include <sys/systime.h>
#include <ppu-types.h>

namespace {
	const unsigned int PS3_PERF_MAX_TAGS  = 32;
	const u64          PS3_PERF_WINDOW_US = 1000000ULL; /* 1 s */

	struct TagSlot {
		const char *tag;        /* identity — compared by pointer        */
		u64         lastStart;  /* sysTime at last ps3_perf_begin, in µs */
		u64         accum;      /* accumulated µs since last window flush */
		u32         count;      /* call count since last window flush     */
		u8          inUse;      /* slot allocated                          */
		u8          active;     /* ps3_perf_begin without matching end     */
	};

	TagSlot g_slots[PS3_PERF_MAX_TAGS];
	FILE   *g_logFile   = NULL;
	u64     g_lastFlush = 0;
	u32     g_window    = 0;

	u64 nowUs() {
		u64 sec = 0, nsec = 0;
		sysGetCurrentTime(&sec, &nsec);
		return sec * 1000000ULL + nsec / 1000ULL;
	}

	TagSlot *findSlot(const char *tag) {
		/* Linear scan; matched by pointer (callers pass string literals). */
		for (unsigned int i = 0; i < PS3_PERF_MAX_TAGS; ++i) {
			if (g_slots[i].inUse && g_slots[i].tag == tag)
				return &g_slots[i];
		}
		return NULL;
	}

	TagSlot *allocSlot(const char *tag) {
		for (unsigned int i = 0; i < PS3_PERF_MAX_TAGS; ++i) {
			if (!g_slots[i].inUse) {
				g_slots[i].tag      = tag;
				g_slots[i].lastStart = 0;
				g_slots[i].accum    = 0;
				g_slots[i].count    = 0;
				g_slots[i].inUse    = 1;
				g_slots[i].active   = 0;
				return &g_slots[i];
			}
		}
		return NULL; /* table full */
	}

	void flushWindow(u64 now) {
		if (!g_logFile) return;
		++g_window;
		for (unsigned int i = 0; i < PS3_PERF_MAX_TAGS; ++i) {
			TagSlot &s = g_slots[i];
			if (!s.inUse || s.count == 0) continue;
			u64 avg = s.accum / s.count;
			std::fprintf(g_logFile, "%u,%s,%u,%llu,%llu\n",
			             g_window, s.tag, s.count,
			             (unsigned long long)s.accum,
			             (unsigned long long)avg);
			s.accum = 0;
			s.count = 0;
		}
		std::fflush(g_logFile);
		g_lastFlush = now;
	}
} /* namespace */

extern "C" {

void ps3_perf_open(const char *path) {
	if (g_logFile || !path) return;
	g_logFile = std::fopen(path, "w");
	if (!g_logFile) return;
	std::fprintf(g_logFile, "# etr_perf.log window=%llus\n",
	             (unsigned long long)(PS3_PERF_WINDOW_US / 1000000ULL));
	std::fprintf(g_logFile, "# columns: window,<tag>,<calls>,<total_us>,<avg_us>\n");
	std::fflush(g_logFile);
	g_lastFlush = nowUs();
	g_window    = 0;
}

void ps3_perf_close(void) {
	if (!g_logFile) return;
	flushWindow(nowUs());
	std::fclose(g_logFile);
	g_logFile = NULL;
}

void ps3_perf_begin(const char *tag) {
	if (!g_logFile || !tag) return;
	TagSlot *s = findSlot(tag);
	if (!s) s = allocSlot(tag);
	if (!s) return;
	s->lastStart = nowUs();
	s->active    = 1;
}

void ps3_perf_end(const char *tag) {
	if (!g_logFile || !tag) return;
	u64 now = nowUs();
	TagSlot *s = findSlot(tag);
	if (!s || !s->active) return;
	s->accum += now - s->lastStart;
	s->count += 1;
	s->active  = 0;
	if (now - g_lastFlush >= PS3_PERF_WINDOW_US)
		flushWindow(now);
}

} /* extern "C" */

#endif /* OS_PS3 */
