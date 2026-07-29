/* --------------------------------------------------------------------
EXTREME TUXRACER - DEMO_MODE screenshot hook

Neutral declaration; PS3 impl in src/ps3/source/screenshot_ps3.cpp.
Only referenced under #ifdef DEMO_MODE (PS3-only demo path).
---------------------------------------------------------------------*/
#ifndef SCREENSHOT_H
#define SCREENSHOT_H

/* Capture the current framebuffer to a PNG at `path`.
 *   maxW == 0 && maxH == 0  -> native framebuffer resolution.
 *   otherwise              -> bilinear, aspect-preserving fit into (maxW x maxH). */
void demoScreenshot(const char* path, int maxW, int maxH);

#endif // SCREENSHOT_H
