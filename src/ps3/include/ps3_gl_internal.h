/* PS3 GL shim — internal interface between the shim and the PS3 window
 * backend. The window backend calls ps3_gl_init() once after init_screen() so
 * the shim can load its shader, create the white fallback texture and allocate
 * the immediate-mode vertex buffer.
 */
#ifndef PS3_GL_INTERNAL_H
#define PS3_GL_INTERNAL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Called once after init_screen()/setRenderTarget(). Loads the embedded etr2d
 * vertex/fragment programs, creates the 1x1 white fallback texture, allocates
 * the immediate-mode vertex buffer and initialises the matrix stacks + GL
 * state to sane defaults. */
void ps3_gl_init(void);

/* Submit compatible immediate-mode primitives retained for batching. Called
 * by the window backend immediately before presenting the framebuffer. */
void ps3_gl_flush_pending(void);

/* Forget cached RSX register/program state after an external surface change. */
void ps3_gl_invalidate_rsx_state(void);

#ifdef __cplusplus
}
#endif

#endif /* PS3_GL_INTERNAL_H */
