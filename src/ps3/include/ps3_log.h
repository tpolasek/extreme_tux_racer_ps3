/* PS3 performance log — aggregated tag-based timer writing a CSV to disk.
 *
 * Usage from anywhere in the game or PS3 backend:
 *
 *     TIMER_START("MAIN_LOOP");
 *     ... work ...
 *     TIMER_END("MAIN_LOOP");
 *
 * Each TIMER_END adds the elapsed µs to that tag's accumulator and bumps
 * its call count. Once per wall-clock window (default 1 s) every active
 * tag emits one CSV row and the accumulators reset. This keeps the log
 * bounded — five tags produce ~5 lines/second instead of one line per
 * call. The file is fflushed at each window flush and on close.
 *
 * Off-PS3 (Linux build) TIMER_START / TIMER_END expand to nothing.
 *
 * The log file is opened with ps3_perf_open() and closed with
 * ps3_perf_close(); every other call is a no-op while the file is not
 * open, so failed-open is safe and call sites need no guards.
 */
#ifndef PS3_LOG_H
#define PS3_LOG_H

/* bh.h derives OS_PS3 from __PPU__ for game sources, but PS3 backend TUs
 * (ps3_gl.cpp, n_window_ps3.cpp) don't include bh.h — so gate on __PPU__
 * directly. ppu-g++ always defines it; it's never set on the Linux build. */
#ifdef __PPU__

#ifdef __cplusplus
extern "C" {
#endif

/* Open `path` for writing (truncates). Writes the CSV header. If fopen
 * fails, all subsequent calls become no-ops. */
void ps3_perf_open (const char *path);
/* Flush + close the log. Safe to call when not open. */
void ps3_perf_close(void);
/* Record the start of a region tagged `tag`. Callers should pass string
 * literals — slots are matched by pointer equality, not strcmp. */
void ps3_perf_begin(const char *tag);
/* Record the end of a region tagged `tag`. Adds elapsed µs since the
 * matching ps3_perf_begin() to the tag's accumulator and bumps its call
 * count. May trigger a periodic window flush. */
void ps3_perf_end  (const char *tag);

#ifdef __cplusplus
}
#endif

#define TIMER_START(tag) ps3_perf_begin(tag)
#define TIMER_END(tag)   ps3_perf_end(tag)

#else /* !__PPU__ */

#define TIMER_START(tag) ((void)0)
#define TIMER_END(tag)   ((void)0)

#endif /* __PPU__ */

#endif /* PS3_LOG_H */
