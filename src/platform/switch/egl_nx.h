// OpenGL ES 3 on the Switch's own native window: libnx's NWindow with Mesa's EGL, the way the
// devkitPro OpenGL examples do it, with no SDL video driver in between. Like nx_sys.h this
// includes no libnx header; egl_nx.cpp is the one that does.
#pragma once

namespace nx {

// Creates the EGL display, an OpenGL ES 3 context and the window surface, and makes the context
// current (swaps wait for the display's refresh). Gives the surface's size. False if any step
// fails; glError() then says which (the details are also in smas_port.log).
bool glStart(int* w, int* h);

// The context (non-null while started), for the code that only needs to know there is one.
void* glContext();

// Shows the frame drawn so far (waits for the display's refresh).
void glSwap();

// Ends the context, the surface and the display.
void glStop();

// Is the context still the one current on this thread (and its surface the drawing one)?
bool glIsCurrent();
// Makes it current again; false if the driver refuses.
bool glMakeCurrent();

// The last failure, as a sentence for the player and the log.
const char* glError();

}  // namespace nx
