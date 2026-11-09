// The Nintendo Switch's own services (libnx), behind a small interface that
// includes no libnx header: libnx defines names the app uses too (PadState,
// ...), so only nx_sys.cpp may include <switch.h>.
#pragma once
#include <cstddef>
#include <cstdint>

namespace nx {

// Starts what the port needs from the system (romfs, the shared system fonts,
// the controllers, the CPU clock); false if it cannot even start.
bool init();
void shutdown();

// The applet main loop (false once the system asks the program to close).
bool mainLoop();

// Controllers: 0 is player 1 (the handheld's own pad or controller 1), 1..7
// controllers 2..8. `buttons` already carries the app's standard layout, by
// position on the pad (see Input): bit 0 = A (the bottom face button: the
// Switch's B), 1 = B (right: the Switch's A), 2 = X (left: Y), 3 = Y (top: X),
// 4 = LB (L), 5 = RB (R), 6 = Back (-), 7 = Start (+), 8 = left stick press,
// 9 = right stick press, 10..13 = Up Down Left Right, 14 = LT (ZL), 15 = RT (ZR).
// Sticks are -32767..32767, positive = right / up.
constexpr int kPads = 8;
struct Pad {
  bool connected = false;
  uint32_t buttons = 0;
  int lx = 0, ly = 0, rx = 0, ry = 0;
};
void padUpdate();  // once per pass
Pad padGet(int index);

// The system's shared fonts (PlSharedFontType: 0 standard, 1 simplified
// Chinese, 2 traditional Chinese, 3 Korean, 4 Nintendo extended symbols,
// 5 simplified Chinese extended). Null if that one isn't there.
const void* sharedFont(int type, size_t* size);

// The screen's size in the console's current mode: 1280x720 handheld, 1920x1080
// docked.
void displaySize(int* w, int* h);

// The touch screen (read directly: the port has no SDL window). Finger changes since the last
// call, positions as 0..1 across the screen.
struct TouchEvent {
  enum Kind { Down, Move, Up } kind;
  int64_t id;
  float x, y;
};
int touchPoll(TouchEvent* out, int max);  // once per pass; how many events were written

// False while the system has sent the program to the background (HOME menu and the like).
bool inFocus();

// The CPU's clock in Hz now (0 if it can't be read); keepOverclock sets the chosen clock again if the
// system has put it back (it does nothing at "stock").
unsigned cpuClockHz();
void keepOverclock();

// The CPU clock the player chose: 0 stock (the system's own, 1020 MHz), 1 = 1224 MHz, 2 = 1581 MHz,
// 3 = 1785 MHz. Off (stock) is the default; the Options menu cycles it.
void setCpuClock(int step);

// Shows a message on the console screen and waits for + (SDL must be shut
// down first); for errors the player has to be told about.
void showFatal(const char* message);

}  // namespace nx
