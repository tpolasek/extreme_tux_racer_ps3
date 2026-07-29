/* Graphics / RSX assertion helper.
 *
 * Standard assert() writes to stderr, which on the PS3 is not wired to
 * anything visible — failures vanish silently. RPCS3 is permissive about
 * the kind of alignment / coherency / primitive-count mistakes real RSX
 * hardware faults on, so a port that runs cleanly under emulation can
 * still ship broken (or slow) on real iron.
 *
 * GFX_ASSERT routes the failure to TTY (visible both in RPCS3's log and
 * a real console's TTY capture) and halts the PPU thread, leaving the
 * failing frame on screen and the RSX ring idle — easier to read the
 * capture or attach a debugger than chasing corruption through the next
 * hundred draws.
 *
 * Halt is the default. -DGFX_ASSERT_NO_HALT at compile time logs and
 * continues instead (handy when you suspect many minor violations and
 * want to see them all in one run).
 */
#ifndef PS3_GFX_ASSERT_H
#define PS3_GFX_ASSERT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called by GFX_ASSERT — formats "[etr] GFX ASSERT FAILED: ..." to TTY
 * and then halts (unless GFX_ASSERT_NO_HALT). Exposed so non-macro
 * callers can route through the same sink. */
void ps3_gfx_assert_failed(const char *expr, const char *file, int line,
                           const char *msg);

#ifdef __cplusplus
}
#endif

/* True iff `ptr` is aligned to `align` bytes. `align` must be a power of
 * two. Works on both pointers and integers (e.g. RSX byte offsets). */
#define GFX_IS_ALIGNED(ptr, align) \
    (((uintptr_t)(ptr) & ((uintptr_t)(align) - 1)) == 0)

/* True iff `v` is a positive multiple of `m` (m > 0). */
#define GFX_IS_MULT(v, m) (((m) > 0) && ((v) % (m) == 0))

/* GFX_ASSERT(expr, msg) — msg may be "" or a short static string giving
 * the *why* (e.g. "RSX needs 16-byte stride"). The expression text and
 * source location are emitted automatically. */
#define GFX_ASSERT(expr, msg) \
    do { \
        if (!(expr)) { \
            ps3_gfx_assert_failed(#expr, __FILE__, __LINE__, (msg)); \
        } \
    } while (0)

/* Form without a custom message. */
#define GFX_ASSERT_NOMSG(expr) GFX_ASSERT(expr, "")

/* GFX_ASSERT_ALIGNED(ptr, align) — common case; the failure message
 * names the pointer, the required alignment and "misaligned RSX data". */
#define GFX_ASSERT_ALIGNED(ptr, align) \
    do { \
        if (!GFX_IS_ALIGNED((ptr), (align))) { \
            ps3_gfx_assert_failed(#ptr " aligned to " #align, \
                                  __FILE__, __LINE__, \
                                  "misaligned RSX data — RSX faults where RPCS3 tolerates"); \
        } \
    } while (0)

#endif /* PS3_GFX_ASSERT_H */
