// libnx glue: see nx_sys.h. (The only file that includes <switch.h>.)
#include "platform/switch/nx_sys.h"
#include <switch.h>
#include <cstdio>
#include <cstring>

void logf(const char* fmt, ...);  // (the app's log file, smas_port.log: see app/app.cpp; global, not in nx)

namespace nx {

namespace {
PadState g_pads[kPads];
bool g_padsInit = false;
bool g_romfs = false, g_pl = false;
bool g_clkrst = false, g_pcv = false;
ClkrstSession g_cpuSession;
bool g_cpuSessionOpen = false;
PlFontData g_fonts[6];
int g_fontState[6] = {};  // 0 not asked, 1 there, -1 missing
}  // namespace

bool init() {
  g_romfs = R_SUCCEEDED(romfsInit());  // (the title font is in it; the port runs without)
  g_pl = R_SUCCEEDED(plInitialize(PlServiceType_User));

  // all controller kinds, up to eight players
  padConfigureInput(kPads, HidNpadStyleSet_NpadStandard);
  // A single Joy-Con is held sideways: the system then reports its buttons for that grip (without this
  // they come as if it were held upright, and the face buttons are turned a quarter from where they are).
  hidSetNpadJoyHoldType(HidNpadJoyHoldType_Horizontal);
  padInitializeDefault(&g_pads[0]);  // controller 1 and the handheld's own pad
  for (int i = 1; i < kPads; i++) padInitializeWithMask(&g_pads[i], 1ULL << i);
  g_padsInit = true;
  padUpdate();
  hidInitializeTouchScreen();
  return true;
}

void shutdown() {
  setCpuClock(0);
  if (g_pl) plExit();
  if (g_romfs) romfsExit();
  g_pl = g_romfs = false;
}

bool mainLoop() { return appletMainLoop(); }

void padUpdate() {
  if (!g_padsInit) return;
  for (int i = 0; i < kPads; i++) padUpdate(&g_pads[i]);
}

Pad padGet(int index) {
  Pad out;
  if (!g_padsInit || index < 0 || index >= kPads) return out;
  PadState* p = &g_pads[index];
  out.connected = padIsConnected(p);
  if (!out.connected) return out;
  u64 b = padGetButtons(p);
  // A single Joy-Con has two small shoulder buttons, SL and SR, in place of L and R.
  const u32 style = padGetStyleSet(p);
  const bool single = (style & (HidNpadStyleTag_NpadJoyLeft | HidNpadStyleTag_NpadJoyRight)) != 0;
  if (single) {
    if (b & (HidNpadButton_LeftSL | HidNpadButton_RightSL)) b |= HidNpadButton_L;
    if (b & (HidNpadButton_LeftSR | HidNpadButton_RightSR)) b |= HidNpadButton_R;
  }
  // (the first presses on controllers 1 and 2 go to the log, with the kind of controller: what the system
  // reports for a button that does not do what it should)
  static int logged = 0;
  if (index < 2 && logged < 80) {
    const u64 down = padGetButtonsDown(p);
    if (down) {
      logged++;
      logf("pad %d: style 0x%x%s, pressed 0x%llx, held 0x%llx", index, (unsigned)style, single ? " (single Joy-Con)" : "",
           (unsigned long long)down, (unsigned long long)b);
    }
  }
  auto bit = [&](u64 mask, int n) { return (b & mask) ? (1u << n) : 0u; };
  out.buttons = bit(HidNpadButton_B, 0) | bit(HidNpadButton_A, 1) | bit(HidNpadButton_Y, 2) |
                bit(HidNpadButton_X, 3) | bit(HidNpadButton_L, 4) | bit(HidNpadButton_R, 5) |
                bit(HidNpadButton_Minus, 6) | bit(HidNpadButton_Plus, 7) | bit(HidNpadButton_StickL, 8) |
                bit(HidNpadButton_StickR, 9) | bit(HidNpadButton_Up, 10) | bit(HidNpadButton_Down, 11) |
                bit(HidNpadButton_Left, 12) | bit(HidNpadButton_Right, 13) | bit(HidNpadButton_ZL, 14) |
                bit(HidNpadButton_ZR, 15);
  HidAnalogStickState l = padGetStickPos(p, 0), r = padGetStickPos(p, 1);
  out.lx = l.x;
  out.ly = l.y;
  out.rx = r.x;
  out.ry = r.y;
  return out;
}

const void* sharedFont(int type, size_t* size) {
  if (type < 0 || type > 5 || !g_pl) return nullptr;
  if (g_fontState[type] == 0) {
    g_fontState[type] = R_SUCCEEDED(plGetSharedFontByType(&g_fonts[type], (PlSharedFontType)type)) ? 1 : -1;
  }
  if (g_fontState[type] != 1) return nullptr;
  if (size) *size = g_fonts[type].size;
  return g_fonts[type].address;
}

int touchPoll(TouchEvent* out, int max) {
  static bool down[16] = {};
  static u32 ids[16] = {};
  static float xs[16] = {}, ys[16] = {};
  HidTouchScreenState st = {};
  if (hidGetTouchScreenStates(&st, 1) < 1) return 0;
  int n = 0;
  const int count = st.count < 16 ? (int)st.count : 16;
  bool seen[16] = {};
  for (int i = 0; i < count; i++) {
    const HidTouchState& t = st.touches[i];
    const float x = (float)t.x / 1280.0f, y = (float)t.y / 720.0f;
    int slot = -1;
    for (int k = 0; k < 16; k++)
      if (down[k] && ids[k] == t.finger_id) slot = k;
    if (slot < 0) {
      for (int k = 0; k < 16 && slot < 0; k++)
        if (!down[k]) slot = k;
      if (slot < 0) continue;
      down[slot] = true;
      ids[slot] = t.finger_id;
      xs[slot] = x;
      ys[slot] = y;
      seen[slot] = true;
      if (n < max) out[n++] = {TouchEvent::Down, (int64_t)t.finger_id, x, y};
    } else {
      seen[slot] = true;
      if ((xs[slot] != x || ys[slot] != y) && n < max) out[n++] = {TouchEvent::Move, (int64_t)t.finger_id, x, y};
      xs[slot] = x;
      ys[slot] = y;
    }
  }
  for (int k = 0; k < 16; k++) {
    if (down[k] && !seen[k]) {
      down[k] = false;
      if (n < max) out[n++] = {TouchEvent::Up, (int64_t)ids[k], xs[k], ys[k]};
    }
  }
  return n;
}

bool inFocus() { return appletGetFocusState() == AppletFocusState_InFocus; }

void displaySize(int* w, int* h) {
  const bool handheld = appletGetOperationMode() == AppletOperationMode_Handheld;
  if (w) *w = handheld ? 1280 : 1920;
  if (h) *h = handheld ? 720 : 1080;
}

unsigned cpuClockHz() {
  u32 hz = 0;
  if (g_cpuSessionOpen) {
    if (R_FAILED(clkrstGetClockRate(&g_cpuSession, &hz))) hz = 0;
  } else if (g_pcv) {
    if (R_FAILED(pcvGetClockRate(PcvModule_CpuBus, &hz))) hz = 0;
  }
  return hz;
}

// The clock the player chose, in Hz (0 = stock).
static u32 g_targetHz = 0;

// The system may put the clock back (a change of performance mode, a dock): set it again.
void keepOverclock() {
  if (!g_targetHz) return;
  const u32 hz = cpuClockHz();
  if (!hz || hz >= g_targetHz) return;
  if (g_cpuSessionOpen) clkrstSetClockRate(&g_cpuSession, g_targetHz);
  else if (g_pcv) pcvSetClockRate(PcvModule_CpuBus, g_targetHz);
  logf("CPU clock was %u MHz: set to %u MHz again (now %u MHz)", hz / 1000000, g_targetHz / 1000000, cpuClockHz() / 1000000);
}

void setCpuClock(int step) {
  static const u32 kSteps[4] = {0, 1224000000, 1581000000, 1785000000};
  if (step < 0 || step > 3) step = 0;
  const u32 hz = kSteps[step];
  g_targetHz = hz;
  if (hz) {
    if (!g_cpuSessionOpen && !g_pcv) {
      if (hosversionAtLeast(8, 0, 0)) {
        if (R_SUCCEEDED(clkrstInitialize())) {
          g_clkrst = true;
          if (R_SUCCEEDED(clkrstOpenSession(&g_cpuSession, PcvModuleId_CpuBus, 3))) {
            g_cpuSessionOpen = true;
          } else {
            logf("CPU clock: the clock session could not be opened (the CPU stays at the system's speed)");
            return;
          }
        } else {
          logf("CPU clock: the clock service could not be started (the CPU stays at the system's speed)");
          return;
        }
      } else if (R_SUCCEEDED(pcvInitialize())) {
        g_pcv = true;
      } else {
        logf("CPU clock: the clock service could not be started (the CPU stays at the system's speed)");
        return;
      }
    }
    Result rc = 0;
    if (g_cpuSessionOpen) rc = clkrstSetClockRate(&g_cpuSession, hz);
    else if (g_pcv) rc = pcvSetClockRate(PcvModule_CpuBus, hz);
    logf("CPU clock: asked for %u MHz (result 0x%x), now %u MHz", hz / 1000000, (unsigned)rc, cpuClockHz() / 1000000);
  } else {
    // stock: the system's own clock (1020 MHz). Set it back, then let go of the clock (closing the session
    // hands it to the system again).
    if (g_cpuSessionOpen) {
      clkrstSetClockRate(&g_cpuSession, 1020000000);
      clkrstCloseSession(&g_cpuSession);
    }
    g_cpuSessionOpen = false;
    if (g_clkrst) clkrstExit();
    g_clkrst = false;
    if (g_pcv) {
      pcvSetClockRate(PcvModule_CpuBus, 1020000000);
      pcvExit();
    }
    g_pcv = false;
    logf("CPU clock: stock (the system's own)");
  }
}

void showFatal(const char* message) {
  consoleInit(nullptr);
  std::printf("\x1b[1;1HSuper Mario All-Stars\n\n%s\n\nPress + to exit.\n", message);
  consoleUpdate(nullptr);
  PadState pad;
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&pad);
  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
    consoleUpdate(nullptr);
  }
  consoleExit(nullptr);
}

}  // namespace nx
