/* PS3 GLX shim — exists solely so <GL/glx.h> (included by bh.h on non-PS3, and
 * referenced indirectly) resolves. Only glXGetProcAddressARB is actually
 * called by the game (ogl.cpp::InitOpenglExtensions); it returns NULL and the
 * game treats the missing extension gracefully. The other entry points are
 * declared so future code referencing them links, but are unused on PS3.
 */
#ifndef PS3_GL_SHIM_GLX_H
#define PS3_GL_SHIM_GLX_H

#ifdef __cplusplus
extern "C" {
#endif

#include <GL/gl.h>

/* Opaque types (the PS3 backend keeps its state in rsxutil globals). */
typedef struct GLXContextRec *GLXContext;
typedef struct GLXFBConfigRec *GLXFBConfig;

void *glXGetProcAddressARB(const GLubyte *procName);

/* Stubs (unused on PS3; declared for link completeness). */
int   glXChooseFBConfig(void *dpy, int screen, const int *attribs, int *nelem);
void *glXGetVisualFromFBConfig(void *dpy, GLXFBConfig config);
GLXContext glXCreateNewContext(void *dpy, GLXFBConfig config, int renderType,
                               GLXContext shareList, GLboolean direct);
void  glXDestroyContext(void *dpy, GLXContext ctx);
int   glXMakeCurrent(void *dpy, unsigned long drawable, GLXContext ctx);
void  glXSwapBuffers(void *dpy, unsigned long drawable);

#ifdef __cplusplus
}
#endif

#endif /* PS3_GL_SHIM_GLX_H */
