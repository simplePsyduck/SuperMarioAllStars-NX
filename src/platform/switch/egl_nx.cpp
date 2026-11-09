// EGL on libnx's native window: see egl_nx.h.
#include "platform/switch/egl_nx.h"
#include <switch.h>
#include <EGL/egl.h>
#include <cstdarg>
#include <cstdio>
#include <string>

#ifndef EGL_OPENGL_ES3_BIT
#define EGL_OPENGL_ES3_BIT 0x00000040  // (EGL 1.5; EGL_OPENGL_ES3_BIT_KHR in older headers)
#endif

void logf(const char* fmt, ...);  // (the app's log file, smas_port.log: see app/app.cpp)

namespace nx {

namespace {

EGLDisplay g_display = EGL_NO_DISPLAY;
EGLContext g_context = EGL_NO_CONTEXT;
EGLSurface g_surface = EGL_NO_SURFACE;
std::string g_error;
int g_swapFailures = 0;
bool g_firstSwap = true;

// The size the picture is drawn at: the layer's own 1280x720 (docked, the system scales it up).
constexpr int kWidth = 1280, kHeight = 720;

const char* str(const char* s) { return s ? s : "?"; }

// Records what went wrong (with EGL's own error code) in the log and for glError().
bool fail(const char* what) {
  char buf[200];
  std::snprintf(buf, sizeof buf, "%s (EGL error 0x%04x)", what, (unsigned)eglGetError());
  g_error = buf;
  logf("EGL: %s", buf);
  return false;
}

int configValue(EGLConfig c, EGLint attr) {
  EGLint v = -1;
  eglGetConfigAttrib(g_display, c, attr, &v);
  return v;
}

}  // namespace

bool glStart(int* w, int* h) {
  glStop();
  g_error.clear();
  g_swapFailures = 0;
  g_firstSwap = true;

  g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (g_display == EGL_NO_DISPLAY) return fail("no EGL display");
  EGLint major = 0, minor = 0;
  if (!eglInitialize(g_display, &major, &minor)) {
    g_display = EGL_NO_DISPLAY;
    return fail("eglInitialize failed");
  }
  logf("EGL %d.%d, vendor \"%s\", version \"%s\", client APIs \"%s\"", (int)major, (int)minor,
       str(eglQueryString(g_display, EGL_VENDOR)), str(eglQueryString(g_display, EGL_VERSION)),
       str(eglQueryString(g_display, EGL_CLIENT_APIS)));
  if (!eglBindAPI(EGL_OPENGL_ES_API)) return fail("eglBindAPI(OpenGL ES) failed");

  // The configuration: an OpenGL ES 3 window surface in RGBA8888 (the layer's own format); if the
  // driver offers none, progressively less is asked for.
  struct Try {
    EGLint renderable;
    bool rgba8888;
    const char* name;
  };
  const Try tries[] = {
      {EGL_OPENGL_ES3_BIT, true, "OpenGL ES 3, RGBA8888"},
      {EGL_OPENGL_ES3_BIT, false, "OpenGL ES 3, any colour format"},
      {EGL_OPENGL_ES2_BIT, true, "OpenGL ES 2 config, RGBA8888"},
      {EGL_OPENGL_ES2_BIT, false, "OpenGL ES 2 config, any colour format"},
  };
  EGLConfig config = nullptr;
  for (const Try& t : tries) {
    EGLint attr[24];
    int n = 0;
    attr[n++] = EGL_SURFACE_TYPE;
    attr[n++] = EGL_WINDOW_BIT;
    attr[n++] = EGL_RENDERABLE_TYPE;
    attr[n++] = t.renderable;
    if (t.rgba8888) {
      attr[n++] = EGL_RED_SIZE;
      attr[n++] = 8;
      attr[n++] = EGL_GREEN_SIZE;
      attr[n++] = 8;
      attr[n++] = EGL_BLUE_SIZE;
      attr[n++] = 8;
      attr[n++] = EGL_ALPHA_SIZE;
      attr[n++] = 8;
    }
    attr[n++] = EGL_NONE;
    EGLConfig c = nullptr;
    EGLint count = 0;
    if (eglChooseConfig(g_display, attr, &c, 1, &count) && count > 0 && c) {
      config = c;
      logf("EGL config: %s", t.name);
      break;
    }
    logf("EGL config not offered: %s", t.name);
  }
  if (!config) return fail("the driver offers no usable OpenGL ES window configuration");
  logf("EGL config: red %d green %d blue %d alpha %d, depth %d, stencil %d, renderable 0x%x",
       configValue(config, EGL_RED_SIZE), configValue(config, EGL_GREEN_SIZE),
       configValue(config, EGL_BLUE_SIZE), configValue(config, EGL_ALPHA_SIZE),
       configValue(config, EGL_DEPTH_SIZE), configValue(config, EGL_STENCIL_SIZE),
       (unsigned)configValue(config, EGL_RENDERABLE_TYPE));

  const EGLint ctxAttr[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  g_context = eglCreateContext(g_display, config, EGL_NO_CONTEXT, ctxAttr);
  if (g_context == EGL_NO_CONTEXT) return fail("could not create an OpenGL ES 3 context");

  // The native window must be told its size before the surface is made: Mesa takes the buffers'
  // size from it, and a window that was never given one (the first run's log said "EGL surface
  // 0x0") has nothing to show, which is a black screen. (The system scales the layer to the
  // screen when docked.)
  NWindow* win = nwindowGetDefault();
  nwindowSetDimensions(win, kWidth, kHeight);
  logf("native window set to %dx%d", kWidth, kHeight);
  g_surface = eglCreateWindowSurface(g_display, config, (EGLNativeWindowType)win, nullptr);
  if (g_surface == EGL_NO_SURFACE) return fail("could not create the window surface");
  if (!eglMakeCurrent(g_display, g_surface, g_surface, g_context)) return fail("eglMakeCurrent failed");
  if (!eglSwapInterval(g_display, 1)) logf("EGL: eglSwapInterval(1) was refused (error 0x%04x)", (unsigned)eglGetError());

  EGLint sw = 0, sh = 0;
  eglQuerySurface(g_display, g_surface, EGL_WIDTH, &sw);
  eglQuerySurface(g_display, g_surface, EGL_HEIGHT, &sh);
  logf("EGL surface %dx%d, context current: %d", (int)sw, (int)sh, (int)glIsCurrent());
  if (sw <= 0 || sh <= 0) {
    // (Mesa may not say until the first buffer: the size just set is what the buffers have)
    sw = kWidth;
    sh = kHeight;
    logf("EGL reported no surface size yet: using %dx%d", (int)sw, (int)sh);
  }
  if (w) *w = sw;
  if (h) *h = sh;
  return true;
}

void* glContext() { return g_context == EGL_NO_CONTEXT ? nullptr : (void*)g_context; }

void glSwap() {
  if (g_display == EGL_NO_DISPLAY || g_surface == EGL_NO_SURFACE) return;
  if (!eglSwapBuffers(g_display, g_surface) && g_swapFailures < 5) {
    g_swapFailures++;
    logf("EGL: eglSwapBuffers failed (error 0x%04x)", (unsigned)eglGetError());
  }
  if (g_firstSwap) {  // (the size Mesa reports once a buffer has been through)
    g_firstSwap = false;
    EGLint sw = 0, sh = 0;
    eglQuerySurface(g_display, g_surface, EGL_WIDTH, &sw);
    eglQuerySurface(g_display, g_surface, EGL_HEIGHT, &sh);
    logf("EGL surface after the first swap: %dx%d", (int)sw, (int)sh);
  }
}

void glStop() {
  if (g_display != EGL_NO_DISPLAY) {
    eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (g_surface != EGL_NO_SURFACE) eglDestroySurface(g_display, g_surface);
    if (g_context != EGL_NO_CONTEXT) eglDestroyContext(g_display, g_context);
    eglTerminate(g_display);
  }
  g_display = EGL_NO_DISPLAY;
  g_context = EGL_NO_CONTEXT;
  g_surface = EGL_NO_SURFACE;
}

bool glIsCurrent() {
  return g_display != EGL_NO_DISPLAY && eglGetCurrentContext() == g_context && eglGetCurrentSurface(EGL_DRAW) == g_surface;
}

bool glMakeCurrent() {
  if (g_display == EGL_NO_DISPLAY) return false;
  return eglMakeCurrent(g_display, g_surface, g_surface, g_context) != EGL_FALSE;
}

const char* glError() { return g_error.c_str(); }

}  // namespace nx
