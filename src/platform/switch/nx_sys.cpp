// libnx glue: see nx_sys.h. (The only file that includes <switch.h>.)
#include "platform/switch/nx_sys.h"
#include <switch.h>
#include <cstdio>
#include <cstring>

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

const u32 kBoostHz = 1785000000;  // the CPU's top clock
}  // namespace

bool init() {
  g_romfs = R_SUCCEEDED(romfsInit());  // (the title font is in it; the port runs without)
  g_pl = R_SUCCEEDED(plInitialize(PlServiceType_User));

  // all controller kinds, up to eight players
  padConfigureInput(kPads, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&g_pads[0]);  // controller 1 and the handheld's own pad
  for (int i = 1; i < kPads; i++) padInitializeWithMask(&g_pads[i], 1ULL << i);
  g_padsInit = true;
  padUpdate();
  return true;
}

void shutdown() {
  setOverclock(false);
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
  const u64 b = padGetButtons(p);
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

void displaySize(int* w, int* h) {
  const bool handheld = appletGetOperationMode() == AppletOperationMode_Handheld;
  if (w) *w = handheld ? 1280 : 1920;
  if (h) *h = handheld ? 720 : 1080;
}

void setOverclock(bool on) {
  if (on) {
    if (g_cpuSessionOpen || g_pcv) return;
    if (hosversionAtLeast(8, 0, 0)) {
      if (R_SUCCEEDED(clkrstInitialize())) {
        g_clkrst = true;
        if (R_SUCCEEDED(clkrstOpenSession(&g_cpuSession, PcvModuleId_CpuBus, 3))) {
          g_cpuSessionOpen = true;
          clkrstSetClockRate(&g_cpuSession, kBoostHz);
        }
      }
    } else if (R_SUCCEEDED(pcvInitialize())) {
      g_pcv = true;
      pcvSetClockRate(PcvModule_CpuBus, kBoostHz);
    }
  } else {
    // (closing the session hands the clock back to the system)
    if (g_cpuSessionOpen) clkrstCloseSession(&g_cpuSession);
    g_cpuSessionOpen = false;
    if (g_clkrst) clkrstExit();
    g_clkrst = false;
    if (g_pcv) {
      pcvSetClockRate(PcvModule_CpuBus, 1020000000);
      pcvExit();
    }
    g_pcv = false;
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
