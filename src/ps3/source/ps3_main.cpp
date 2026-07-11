/* ETR PS3 EBOOT entry point.
 *
 * The PS3 OS spawns the EBOOT main thread with a small (~128 KiB) stack, which
 * is too small for ETR (font texture uploads, course list parsing, etc. each
 * use sizeable stack frames). Following the chocolate-quake pattern, main()
 * spawns a 2 MiB PPU worker thread that runs the whole game (etr_run) and just
 * joins it. Also declares the process heap param and a sysutil exit callback so
 * the XMB "quit game" signal shuts us down cleanly.
 *
 * etr_run() lives in src/main.cpp (the Linux main() body, refactored into a
 * callable extern "C" entry point guarded by OS_PS3).
 */
#ifdef HAVE_CONFIG_H
#include <etr_config.h>
#endif

#include <sys/thread.h>
#include <sys/process.h>
#include <sys/tty.h>
#include <string.h>

#include "ps3_tty.h"

/* Declare the process parameters: (SDK version, heap size). 0x10000000 == 256
 * MiB — ETR loads many textures/courses; start generous and tune later. */
#define PS3_SDK_VERSION   1001
#define PS3_HEAP_SIZE     (256 * 1024 * 1024)
SYS_PROCESS_PARAM(PS3_SDK_VERSION, PS3_HEAP_SIZE);

/* Worker thread config: 2 MiB stack (game uses deep recursion / large frames),
 * priority 1000 (ordinary app priority). */
#define PS3_GAME_STACK  (2 * 1024 * 1024)
#define PS3_GAME_PRIO   1000

extern "C" int etr_run(void);   /* defined in src/main.cpp */

static sys_ppu_thread_t g_gameThreadId;

static void EtrGameThread(void * /*arg*/) {
	sysTtyTrace("[etr] game thread entry\n");
	int rc = etr_run();
	sysTtyTrace("[etr] game thread exit\n");
	sysThreadExit((u64)(s64)rc);
}

int main(int /*argc*/, char ** /*argv*/) {
	sysTtyTrace("[etr] EBOOT entry\n");

	s32 rc = sysThreadCreate(&g_gameThreadId, EtrGameThread, NULL,
	                         PS3_GAME_PRIO, PS3_GAME_STACK,
	                         THREAD_JOINABLE, "etr_game");
	if (rc != 0) {
		sysTtyTrace("[etr] sysThreadCreate FAILED\n");
		return 1;
	}

	u64 exitCode = 0;
	sysThreadJoin(g_gameThreadId, &exitCode);
	sysTtyTrace("[etr] process exit\n");
	sysProcessExit(1);
	return (int)(s64)exitCode;
}
