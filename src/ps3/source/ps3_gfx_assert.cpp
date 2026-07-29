/* GFX_ASSERT runtime — formats the failure to TTY and halts.
 *
 * Lives in its own TU so the long __FILE__ strings and the halt loop stay
 * out of the hot draw path until a violation actually fires.
 */
#include "ps3_gfx_assert.h"
#include "ps3_tty.h"

#include <stdio.h>
#include <unistd.h>

void ps3_gfx_assert_failed(const char *expr, const char *file, int line,
                           const char *msg) {
    char buf[320];
    int n = snprintf(buf, sizeof(buf),
                     "[etr] GFX ASSERT FAILED: (%s) at %s:%d%s%s\n",
                     expr  ? expr  : "?",
                     file  ? file  : "?",
                     line,
                     (msg && msg[0]) ? "  " : "",
                     (msg && msg[0]) ? msg  : "");
    if (n > 0) {
        if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
        buf[n] = '\0';
        sysTtyWriteStr(buf);
    }

#ifdef GFX_ASSERT_NO_HALT
    sysTtyTrace("[etr] GFX ASSERT: continuing (GFX_ASSERT_NO_HALT)\n");
#else
    /* Park the PPU thread. The last submitted RSX command finishes, then
     * the ring goes idle and the failing frame stays on screen so the
     * TTY capture and the on-screen artifact can be read together. */
    sysTtyTrace("[etr] GFX ASSERT: halting PPU — inspect TTY + frozen frame\n");
    for (;;) usleep(1000000);
#endif
}
