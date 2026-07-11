/* PS3 TTY tracing helper. Writes a NUL-terminated message to /dev/ttyXX via
 * sysTtyWrite so it shows up in the RPCS3 log / real-hardware TTY capture.
 * Used by the PS3 backend sources (ps3_main, n_window_ps3, ps3_gl) for the
 * "[etr] ..." boot/progress traces.
 */
#ifndef PS3_TTY_H
#define PS3_TTY_H

#include <sys/tty.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline void sysTtyWriteStr(const char *msg) {
	u32 written = 0;
	sysTtyWrite(0, msg, (u32)strlen(msg), &written);
}

/* Convenience: write a fixed string (literal or const char*). The ETR sources
 * call sysTtyTrace("[etr] something\n"). */
static inline void sysTtyTrace(const char *msg) {
	sysTtyWriteStr(msg);
}

#ifdef __cplusplus
}
#endif

#endif /* PS3_TTY_H */
