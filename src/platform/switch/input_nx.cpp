// Controllers and keyboards on the Switch: the same model as the Android
// build's (platform/android/input_sdl.h), the controllers read through libnx
// (see nx_sys.h) instead of SDL's GameController API, and the keyboard through
// SDL (USB keyboards).
#include "platform/android/input_sdl.h"
#include <SDL.h>
#include <cstring>
#include "app/canvas.h"
#include "app/vkeys.h"
#include "platform/switch/nx_sys.h"

bool Input::init() {
  pads_.assign(nx::kPads, PadState());
  prevPads_ = pads_;
  for (int i = 0; i < nx::kPads; i++) pads_[i].name = widenUtf8("Switch Controller " + std::to_string(i + 1));
  rescan_ = false;
  return true;
}

void Input::shutdown() {
  pads_.clear();
  clearKeys();
}

void Input::onKey(int vk, bool down) {
  if (vk <= 0 || vk >= 256) return;
  keys_[vk] = down;
  if (down) sticky_[vk] = 2;
}

void Input::clearKeys() {
  std::memset(keys_, 0, sizeof(keys_));
  std::memset(sticky_, 0, sizeof(sticky_));
}

bool Input::anyPad() const {
  for (const PadState& p : pads_)
    if (p.connected) return true;
  return false;
}

// The pads keep their slots: player 1 is the handheld's own pad or controller 1,
// the others controllers 2 to 8; a controller that is not there reads as not connected.
void Input::poll() {
  for (uint8_t& s : sticky_)
    if (s) s--;
  nx::padUpdate();
  if ((int)pads_.size() != nx::kPads) pads_.resize(nx::kPads);
  for (int i = 0; i < nx::kPads; i++) {
    PadState& p = pads_[i];
    const nx::Pad n = nx::padGet(i);
    p.connected = n.connected;
    for (int b = 0; b < 16; b++) p.buttons[b] = (n.buttons >> b & 1) != 0;
    p.buttons[16] = false;
    // (libnx's sticks have up positive; the app's, as SDL's, up negative)
    p.axes[0] = n.lx / 32767.0f;
    p.axes[1] = -n.ly / 32767.0f;
    p.axes[2] = n.rx / 32767.0f;
    p.axes[3] = -n.ry / 32767.0f;
  }
}

bool Input::active(const Binding& b, int padIdx, float dz) const {
  switch (b.type) {
    case Binding::Key: return keyDown(b.code);
    case Binding::PadButton: {
      const PadState* p = pad(padIdx);
      return p && b.code < kPadButtons && p->buttons[b.code];
    }
    case Binding::PadAxisPos: {
      const PadState* p = pad(padIdx);
      return p && b.code < 6 && p->axes[b.code] > dz;
    }
    case Binding::PadAxisNeg: {
      const PadState* p = pad(padIdx);
      return p && b.code < 6 && p->axes[b.code] < -dz;
    }
    case Binding::PadHat: {
      const PadState* p = pad(padIdx);
      return p && p->buttons[10 + (b.code & 3)];
    }
    default: return false;
  }
}

void Input::resetCapture() {
  std::memcpy(prevKeys_, keys_, sizeof(keys_));
  prevPads_ = pads_;
}

bool Input::capture(Binding* out, bool wantPads, int padIdx, float) {
  bool got = false;
  if (!wantPads) {
    for (int k = 1; k < 256 && !got; k++)
      if (keyDown(k) && !prevKeys_[k]) {
        out->type = Binding::Key;
        out->code = (uint16_t)k;
        got = true;
      }
  } else if (const PadState* p = pad(padIdx)) {
    const PadState* q = padIdx < (int)prevPads_.size() ? &prevPads_[padIdx] : nullptr;
    for (int i = 0; i < kPadButtons && !got; i++)
      if (p->buttons[i] && !(q && q->buttons[i])) {
        out->type = Binding::PadButton;
        out->code = (uint16_t)i;
        got = true;
      }
    for (int a = 0; a < 6 && !got; a++) {
      float prev = q ? q->axes[a] : 0;
      if (p->axes[a] > 0.6f && prev <= 0.6f) {
        out->type = Binding::PadAxisPos;
        out->code = (uint16_t)a;
        got = true;
      } else if (p->axes[a] < -0.6f && prev >= -0.6f) {
        out->type = Binding::PadAxisNeg;
        out->code = (uint16_t)a;
        got = true;
      }
    }
  }
  resetCapture();
  return got;
}

int vkFromScancode(int sc) {
  if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return 'A' + (sc - SDL_SCANCODE_A);
  if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return '1' + (sc - SDL_SCANCODE_1);
  if (sc == SDL_SCANCODE_0) return '0';
  if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return VK_F1 + (sc - SDL_SCANCODE_F1);
  if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) return VK_NUMPAD0 + 1 + (sc - SDL_SCANCODE_KP_1);
  switch (sc) {
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER: return VK_RETURN;
    case SDL_SCANCODE_ESCAPE:
    case SDL_SCANCODE_AC_BACK: return VK_ESCAPE;
    case SDL_SCANCODE_BACKSPACE: return VK_BACK;
    case SDL_SCANCODE_TAB: return VK_TAB;
    case SDL_SCANCODE_SPACE: return VK_SPACE;
    case SDL_SCANCODE_UP: return VK_UP;
    case SDL_SCANCODE_DOWN: return VK_DOWN;
    case SDL_SCANCODE_LEFT: return VK_LEFT;
    case SDL_SCANCODE_RIGHT: return VK_RIGHT;
    case SDL_SCANCODE_PAGEUP: return VK_PRIOR;
    case SDL_SCANCODE_PAGEDOWN: return VK_NEXT;
    case SDL_SCANCODE_HOME: return VK_HOME;
    case SDL_SCANCODE_END: return VK_END;
    case SDL_SCANCODE_INSERT: return VK_INSERT;
    case SDL_SCANCODE_DELETE: return VK_DELETE;
    case SDL_SCANCODE_LSHIFT: return VK_LSHIFT;
    case SDL_SCANCODE_RSHIFT: return VK_RSHIFT;
    case SDL_SCANCODE_LCTRL: return VK_LCONTROL;
    case SDL_SCANCODE_RCTRL: return VK_RCONTROL;
    case SDL_SCANCODE_LALT: return VK_LMENU;
    case SDL_SCANCODE_RALT: return VK_RMENU;
    case SDL_SCANCODE_KP_0: return VK_NUMPAD0;
    case SDL_SCANCODE_SEMICOLON: return VK_OEM_1;
    case SDL_SCANCODE_EQUALS: return VK_OEM_PLUS;
    case SDL_SCANCODE_COMMA: return VK_OEM_COMMA;
    case SDL_SCANCODE_MINUS: return VK_OEM_MINUS;
    case SDL_SCANCODE_PERIOD: return VK_OEM_PERIOD;
    case SDL_SCANCODE_SLASH: return VK_OEM_2;
    case SDL_SCANCODE_GRAVE: return VK_OEM_3;
    case SDL_SCANCODE_LEFTBRACKET: return VK_OEM_4;
    case SDL_SCANCODE_BACKSLASH: return VK_OEM_5;
    case SDL_SCANCODE_RIGHTBRACKET: return VK_OEM_6;
    case SDL_SCANCODE_APOSTROPHE: return VK_OEM_7;
    default: return 0;
  }
}
