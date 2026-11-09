// Controllers and keyboards on Android, the same model as Windows' app/input.h:
// keys by Windows virtual-key code (the settings file's), controllers through
// SDL's GameController API in the standard layout (buttons 0-15 = A B X Y LB
// RB Back Start LS RS Up Down Left Right LT RT, axes 0-3 = LX LY RX RY).
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "app/config.h"

constexpr int kPadButtons = 48;

#ifdef __SWITCH__
// (libnx has a PadState of its own: two different types of one name in a program is an ODR violation,
// which link-time optimisation reports and may act on)
#define PadState SmasPadState
#endif
struct PadState {
  bool connected = false;
  bool xinput = false;
  std::wstring name;
  bool buttons[kPadButtons] = {};
  float axes[6] = {};
};

struct _SDL_GameController;

class Input {
public:
  bool init();
  void shutdown();
  void poll();
  void onKey(int vk, bool down);
  void clearKeys();
  void rescanDevices() { rescan_ = true; }

  // (a key down and up again before the next poll, as Back sends it, still reads as down once)
  bool keyDown(int vk) const { return vk >= 0 && vk < 256 && (keys_[vk] || sticky_[vk] > 0); }
  static constexpr int kMaxPads = 8;
  int padCount() const { return (int)pads_.size(); }
  const PadState* pad(int i) const { return (i >= 0 && i < (int)pads_.size()) ? &pads_[i] : nullptr; }
  bool anyPad() const;  // a controller is connected

  bool active(const Binding& b, int padIdx, float deadzone) const;
  bool capture(Binding* out, bool pads, int padIdx, float deadzone);
  void resetCapture();

private:
  bool keys_[256] = {};
  bool prevKeys_[256] = {};
  uint8_t sticky_[256] = {};
  std::vector<PadState> pads_;
  std::vector<PadState> prevPads_;
  std::vector<_SDL_GameController*> ctl_;
  bool rescan_ = true;
  void scan();
};

// SDL scancode -> Windows virtual-key code (0 if unmapped).
int vkFromScancode(int scancode);
