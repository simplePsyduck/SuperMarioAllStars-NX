// Super Mario All-Stars port: the Nintendo Switch side (homebrew .nro, devkitPro
// libnx + SDL2), as app_android.cpp is the Android one: the SDL window and event
// loop, frame pacing, touch, the ROM search. The application itself is
// app/app.cpp. The ROM is looked for in the folder the .nro is in.
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include "app/app.h"
#include "app/sys.h"
#include "app/vkeys.h"
#include "platform/switch/nx_sys.h"

namespace {

std::string g_argv0;  // how the program was started ("sdmc:/switch/<folder>/<name>.nro")

// Refreshes per game frame when the display rate is a whole multiple of the
// game's 60.1 fps, else 0 (the Switch's screen runs at 60 Hz).
int lockedRefreshes(double hz) {
  int k = (int)std::lround(hz / Snes::kFrameRate);
  return k >= 1 && std::fabs(hz / k - Snes::kFrameRate) / Snes::kFrameRate < 0.025 ? k : 0;
}

// The folder of the .nro: the ROM, the settings and the saves live in it.
std::wstring programDir() {
  std::string dir;
  size_t s = g_argv0.find_last_of('/');
  if (s != std::string::npos && s > 0) dir = g_argv0.substr(0, s);
  if (dir.empty() || dir.back() == ':') dir = "sdmc:/switch/SuperMarioAllStars";
  return widenUtf8(dir);
}

bool hasRomExtension(const std::wstring& name) {
  std::wstring lower = name;
  for (wchar_t& c : lower) c = (wchar_t)towlower(c);
  for (const wchar_t* ext : {L".sfc", L".smc", L".swc", L".fig"})
    if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ext) == 0) return true;
  return false;
}

// (an SNES ROM is about 2 MB; a quick size check keeps the search from reading every file of a big folder)
bool plausibleRomSize(const std::wstring& path) {
  FILE* f = openFile(path, "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fclose(f);
  return n >= 0x80000 && n <= (8 << 20);
}

// The size of the picture area: SDL's drawable size; if it reports none (0x0 would leave the
// viewport empty and the screen black), the window's size, else the console's own display size.
void drawableSize(SDL_Window* w, int* outW, int* outH) {
  int dw = 0, dh = 0;
  SDL_GL_GetDrawableSize(w, &dw, &dh);
  if (dw <= 0 || dh <= 0) SDL_GetWindowSize(w, &dw, &dh);
  if (dw <= 0 || dh <= 0) nx::displaySize(&dw, &dh);
  *outW = dw;
  *outH = dh;
}

}  // namespace

void openUrl(const char*) {}  // (no browser to open: the credits' links are only shown)

// (One full-screen window: the aspect and scaling settings apply as it's drawn)
void App::applyDisplayMode() {}

// The ROM: next to the .nro (or in a "roms" folder there).
bool App::findRom(std::wstring* out) {
  for (const std::wstring& dir : {exeDir, pathJoin(exeDir, L"roms")}) {
    std::vector<std::wstring> names;
    if (!listFiles(dir, names)) continue;
    std::sort(names.begin(), names.end());
    for (const std::wstring& n : names) {
      if (!hasRomExtension(n)) continue;
      std::wstring p = pathJoin(dir, n);
      if (!plausibleRomSize(p)) continue;
      std::vector<uint8_t> data;
      if (readAll(p, data) && isSmasRom(data, nullptr)) {
        *out = p;
        return true;
      }
    }
  }
  return false;
}

// (There is no file picker: the ROM is put beside the .nro with a computer.)
void App::pickRom() { menu.notify("Put the ROM in the same folder as this .nro on the SD card", 4.0f); }
void App::pollPick() {}

// Taps and drags on the touch screen: the launcher's cards and buttons, the menus' rows.
// (There is no on-screen pad: the Switch has its own buttons.)
void App::touchFrame() {
  touch.layout((float)video.width(), (float)video.height());
  touch.inGame = false;
  touch.visible = false;
  touch.showFF = touch.showRewind = false;
  float dy = touch.takeDrag(), x, y;
  bool tapped = touch.takeTap(&x, &y);
  if (menu.open()) {
    if (dy != 0) menu.drag(dy);
    if (tapped) menu.tap(x, y);
  } else if (inLauncher && tapped) {
    launcherTap = launcher.tap(x, y);
  }
}

bool App::init() {
  SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
  SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
  exeDir = programDir();
  makeDir(exeDir);
  g_log = openFile(pathJoin(exeDir, L"smas_port.log"), "w");
  logf("starting, program folder %s", narrow(exeDir).c_str());
  configPath = pathJoin(exeDir, L"smas_port.ini");
  saveDir = pathJoin(exeDir, L"saves");
  shotDir = pathJoin(exeDir, L"screenshots");
  makeDir(saveDir);
  if (!fileExists(pathJoin(exeDir, L"no_overclock.txt"))) nx::setOverclock(true);  // (see the README)

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
    logf("SDL_Init failed: %s", SDL_GetError());
    return false;
  }
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  // (the Switch's layer is RGBA8888: ask for exactly that rather than SDL's small defaults)
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
  {
    SDL_version v;
    SDL_GetVersion(&v);
    int dw = 0, dh = 0;
    nx::displaySize(&dw, &dh);
    logf("SDL %d.%d.%d, video driver %s, console screen %dx%d", v.major, v.minor, v.patch,
         SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?", dw, dh);
  }
  window = SDL_CreateWindow("Super Mario All-Stars", 0, 0, 1280, 720, SDL_WINDOW_BORDERLESS | SDL_WINDOW_OPENGL);
  if (!window) {
    logf("window failed: %s", SDL_GetError());
    return false;
  }
  logf("window created");

  if (!cfg.load(configPath)) cfg.save(configPath);
  cfg.vsync = true;

  if (!video.init(window)) {
    logf("video init failed: %s", SDL_GetError());
    return false;
  }
  logf("video ok %dx%d", video.width(), video.height());
  if (!audio.init(cfg.audioLatency)) {
    logf("audio init failed: %s", SDL_GetError());
    menu.notify("Audio device unavailable");
  }
  input.init();
  menu.setCanvas(video.canvas());
  menu.touchUi = true;
  menu.keysHint = L"Up/Down Select      Left/Right Change      A Confirm      B Back";
  launcher.setHint("Left/Right Choose    A Play    B Quit");

  const bool haveRom = loadRom();
  logf(haveRom ? "rom loaded: %s" : "rom not found%s", narrow(romPath).c_str());
  achStore.load(pathJoin(exeDir, L"achievements.ini"));
  launcher.setVersion(kAppVersion);
  launcher.setAllStarsOnly(cfg.launcherAllStars);
  launcher.selectGame(Launcher::cardGame(cfg.lastGame));
  playing = Launcher::cardGame(cfg.lastGame);
  if (haveRom) romReady();
  applyConfig();
  inLauncher = true;
  if (!achStore.noticeShown) {
    achStore.noticeShown = true;
    achStore.save();
    if (cfg.achievements) achWelcome();
  }
  return true;
}

void App::shutdown() {
  saveSram();
  cfg.save(configPath);
  music.shutdown();
  launcher.releaseGpu();
  menu.setCanvas(nullptr);
  audio.shutdown();
  input.shutdown();
  video.shutdown();
  if (window) SDL_DestroyWindow(window);
  window = nullptr;
  if (g_log) fclose(g_log);
  g_log = nullptr;
  SDL_Quit();
}

// One pass per display refresh (SDL_GL_SwapWindow waits for it). The refresh
// rate is the median of recent intervals; on a whole multiple of the game's
// 60.1 fps each game frame gets a steady number of refreshes, else the game
// time owed decides.
void App::run() {
  const Uint64 freq = SDL_GetPerformanceFrequency();
  Uint64 last = SDL_GetPerformanceCounter();
  const double period = 1.0 / Snes::kFrameRate;
  float intervals[48] = {};
  int nIntervals = 0;
  unsigned pass = 0;
  double slowest = 0, spent = 0;  // (the pass times since the last report, for the log)
  unsigned reported = 0;
  logf("main loop starts");
  while (running && nx::mainLoop()) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      const float W = (float)video.width(), H = (float)video.height();
      switch (e.type) {
        case SDL_QUIT: running = false; break;
        case SDL_WINDOWEVENT:
          if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e.window.event == SDL_WINDOWEVENT_RESIZED) {
            int w = 0, h = 0;
            drawableSize(window, &w, &h);
            video.resize(w, h);
          } else if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            // (the system may end the program while it is in the background: keep the game's saves and the settings)
            saveSram();
            cfg.save(configPath);
          } else if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
            last = SDL_GetPerformanceCounter();
            nIntervals = 0;
            pacing.vblanks = 0;
            pacing.accum = 0;
            audio.flush();
          }
          break;
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
          if (e.key.repeat) break;
          int vk = vkFromScancode(e.key.keysym.scancode);
          if (vk) input.onKey(vk, e.type == SDL_KEYDOWN);
          break;
        }
        case SDL_FINGERDOWN: touch.onDown((int64_t)e.tfinger.fingerId, e.tfinger.x * W, e.tfinger.y * H); break;
        case SDL_FINGERMOTION: touch.onMove((int64_t)e.tfinger.fingerId, e.tfinger.x * W, e.tfinger.y * H); break;
        case SDL_FINGERUP: touch.onUp((int64_t)e.tfinger.fingerId, e.tfinger.x * W, e.tfinger.y * H); break;
        default: break;
      }
    }
    if (!running) break;
    // (docking or undocking changes the screen's size: 1280x720 handheld, 1920x1080 docked)
    if ((++pass & 15) == 0) {
      int w = 0, h = 0;
      drawableSize(window, &w, &h);
      if (w > 0 && h > 0 && (w != video.width() || h != video.height())) {
        logf("screen size now %dx%d", w, h);
        video.resize(w, h);
      }
    }
    Uint64 now = SDL_GetPerformanceCounter();
    double dt = (double)(now - last) / freq;
    last = now;
    intervals[nIntervals++ % 48] = (float)dt;
    double hz = 60.0;
    if (nIntervals >= 48) {
      float d[48];
      std::copy(intervals, intervals + 48, d);
      std::nth_element(d, d + 24, d + 48);
      if (d[24] > 0.002f) hz = 1.0 / d[24];
    } else {
      SDL_DisplayMode m;
      if (SDL_GetCurrentDisplayMode(0, &m) == 0 && m.refresh_rate > 0) hz = m.refresh_rate;
    }
    if (pacing.mode != kPaceVsync || lockedRefreshes(hz) != pacing.perFrame) {
      pacing = Pacing();
      pacing.mode = kPaceVsync;
      pacing.perFrame = lockedRefreshes(hz);
      logf("pacing: %.2f Hz display, %d refreshes per frame (0 = uneven)", hz, pacing.perFrame);
    }
    pacing.hz = hz;
    int frames = 0;
    int vb = std::max(1, (int)std::lround(dt * hz));  // refreshes since the last pass
    if (vb > 6) {  // stalled: resume without fast-forwarding
      vb = 0;
      pacing.vblanks = 0;
      pacing.accum = 0;
      frames = 1;
    }
    if (pacing.perFrame) {
      pacing.vblanks += vb;
      while (pacing.vblanks >= pacing.perFrame) {
        pacing.vblanks -= pacing.perFrame;
        frames++;
      }
    } else {
      pacing.accum += vb / hz;
      while (pacing.accum >= period) {
        pacing.accum -= period;
        frames++;
      }
    }
    frames = std::min(frames, 3);
    frame(dt, frames);
    render();
    video.present();
    // (a line in the log every 10 seconds of the first minute: how long the passes take)
    slowest = std::max(slowest, dt);
    spent += dt;
    reported++;
    if (spent >= 10.0 && pass < 4000) {
      logf("passes: %.1f per second, slowest %.1f ms (60 per second needs 16.7)", (double)reported / spent,
           slowest * 1000.0);
      reported = 0;
      slowest = spent = 0;
    }
  }
  logf("main loop ended");
}

int main(int argc, char* argv[]) {
  SDL_SetMainReady();
  if (argc > 0 && argv && argv[0]) g_argv0 = argv[0];
  nx::init();
  int rc = 0;
  {
    std::unique_ptr<App> app = std::make_unique<App>();
    g_app = app.get();
    if (!app->init()) {
      rc = 1;
      const std::string why = SDL_GetError();
      app->shutdown();
      g_app = nullptr;
      app.reset();
      nx::showFatal(("The program could not start (OpenGL ES 3.0 or the window).\n\n" + why +
                     "\n\nSee smas_port.log next to the .nro.")
                        .c_str());
    } else {
      app->run();
      app->shutdown();
      g_app = nullptr;
    }
  }
  nx::shutdown();
  return rc;
}
