// Super Mario All-Stars port: the Nintendo Switch side (homebrew .nro, devkitPro
// libnx + Mesa's OpenGL ES + SDL2 for the sound), as app_android.cpp is the Android
// one: the main loop, frame pacing, touch, the ROM search. The picture is drawn on
// the console's own native window (egl_nx.cpp, no SDL window); the touch screen and
// the focus come from libnx (nx_sys.cpp). The application itself is app/app.cpp.
// The ROM is looked for in the folder the .nro is in.
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include "app/app.h"
#include "app/sys.h"
#include "app/vkeys.h"
#include "platform/switch/egl_nx.h"
#include "platform/switch/jobqueue_nx.h"
#include "platform/switch/nx_sys.h"
#include "platform/switch/perf_nx.h"

namespace {

std::string g_argv0;  // how the program was started ("sdmc:/switch/<folder>/<name>.nro")
std::string g_initError;  // why App::init failed, for the message on the console screen

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

}  // namespace

NxPerf g_nxPerf;

void openUrl(const char*) {}  // (no browser to open: the credits' links are only shown)

// (no network access in the homebrew build: no update check, a new version is a new .nro)
bool httpGet(const char*, std::string& out) {
  out.clear();
  return false;
}
void openFolder(const std::wstring&) {}  // (no file manager: the menus show the folder's path instead)

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
// (covers and the background are PNG or JPG files put in the covers folder with a computer)
void App::pickArt(int) {}
void App::pollArtPick() {}

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
  exeDir = programDir();
  makeDir(exeDir);
  g_log = openFile(pathJoin(exeDir, L"smas_port.log"), "w");
  logf("starting, program folder %s", narrow(exeDir).c_str());
  configPath = pathJoin(exeDir, L"smas_port.ini");
  saveDir = pathJoin(exeDir, L"saves");
  shotDir = pathJoin(exeDir, L"screenshots");
  makeDir(saveDir);
  // (the folders the player puts things in with a computer: the ROM, covers, MSU-1 music packs)
  makeDir(pathJoin(exeDir, L"roms"));
  makeDir(pathJoin(exeDir, L"covers"));
  makeDir(pathJoin(exeDir, L"msu"));

  // (SDL is only for the sound and the clock here: the picture is on the native window below)
  if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
    g_initError = SDL_GetError();
    logf("SDL_Init failed: %s", g_initError.c_str());
    return false;
  }
  {
    SDL_version v;
    SDL_GetVersion(&v);
    int dw = 0, dh = 0;
    nx::displaySize(&dw, &dh);
    logf("SDL %d.%d.%d (audio only), console screen %dx%d", v.major, v.minor, v.patch, dw, dh);
  }

  if (!cfg.load(configPath)) cfg.save(configPath);
  cfg.vsync = true;
  nx::setCpuClock(cfg.cpuClock);  // (stock unless the player chose a higher clock in the Options menu)
  Ppu::queueFactory = makeNxJobQueue;  // (the picture's layers can be mixed on a second core: see Ppu::setThreaded)

  if (!video.init(nullptr)) {
    g_initError = nx::glError();
    if (g_initError.empty()) g_initError = "the OpenGL ES shaders did not build";
    logf("video init failed: %s", g_initError.c_str());
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
  if (g_log) fclose(g_log);
  g_log = nullptr;
  SDL_Quit();
}

// One pass per display refresh (the buffer swap waits for it). The refresh
// rate is the median of recent intervals; on a whole multiple of the game's
// 60.1 fps each game frame gets a steady number of refreshes, else the game
// time owed decides.
// Frame pacing. The Switch's screen is always 60 Hz (handheld and docked) and the swap waits for each
// refresh, so a pass of this loop is a refresh and the game (60.1 fps) runs one frame to it: its speed
// follows the screen's, and the sound's resampler absorbs the 0.16% difference.
//
// The refresh rate is not measured: it was, from the passes' own spacing, and a few slow passes made it
// read 48 Hz, which switched the pacing to its uneven mode and back (the speed-ups and slow-downs).
//
// When a pass runs over a refresh, the next is not made to run two frames to catch up unless the sound
// needs them: that pass would run over as well, and the picture would stay at 30 fps for good. The game
// loses that frame's time instead, which is not seen; the queued sound (64 ms by default) is what says
// whether a frame really is owed.
void App::run() {
  const Uint64 freq = SDL_GetPerformanceFrequency();
  Uint64 last = SDL_GetPerformanceCounter();
  const double hz = 60.0;
  bool hadFocus = true;
  // (what the log says every few seconds)
  struct Stats {
    int passes = 0, over = 0, catchUps = 0;
    double workMs = 0, workMax = 0, frameMs = 0, renderMs = 0, presentMs = 0;
    Uint64 since = 0;
  } st;
  st.since = SDL_GetPerformanceCounter();
  auto ms = [&](Uint64 a, Uint64 b) { return 1000.0 * (double)(b - a) / (double)freq; };
  logf("main loop starts");
  while (running && nx::mainLoop()) {
    // (the focus: the system may end the program while it is in the background, so the game's
    // saves and the settings are kept when it leaves)
    const bool focused = nx::inFocus();
    if (focused != hadFocus) {
      hadFocus = focused;
      logf(focused ? "back in the foreground" : "sent to the background");
      if (!focused) {
        saveSram();
        cfg.save(configPath);
      } else {
        last = SDL_GetPerformanceCounter();
        pacing.vblanks = 0;
        pacing.accum = 0;
        audio.flush();
      }
    }
    // (the touch screen)
    {
      const float W = (float)video.width(), H = (float)video.height();
      nx::TouchEvent te[16];
      const int n = nx::touchPoll(te, 16);
      for (int i = 0; i < n; i++) {
        if (te[i].kind == nx::TouchEvent::Down) touch.onDown(te[i].id, te[i].x * W, te[i].y * H);
        else if (te[i].kind == nx::TouchEvent::Move) touch.onMove(te[i].id, te[i].x * W, te[i].y * H);
        else touch.onUp(te[i].id, te[i].x * W, te[i].y * H);
      }
    }
    const Uint64 passStart = SDL_GetPerformanceCounter();
    const double dt = (double)(passStart - last) / freq;
    last = passStart;
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
      while (pacing.accum >= 1.0 / Snes::kFrameRate) {
        pacing.accum -= 1.0 / Snes::kFrameRate;
        frames++;
      }
    }
    if (frames > 1) {
      // (owed a frame or more: run the extra only while the sound is short of what it needs)
      const int target = std::max(audio.rate() * cfg.audioLatency / 1000, audio.deviceFrames() * 2);
      if (audio.queuedFrames() > target / 2 || !audio.ok()) {
        frames = 1;
        pacing.vblanks = 0;
        pacing.accum = 0;
      } else {
        frames = std::min(frames, 2);
        st.catchUps++;
      }
    }
    const Uint64 t0 = SDL_GetPerformanceCounter();
    frame(dt, frames);
    if (snes && snes->ppu.queueBroken()) {  // (the worker did not answer in a second: go on with one thread)
      logf("video thread: it stopped answering, the picture is mixed on the main thread from here on");
      cfg.threadedVideo = false;
      snes->ppu.setThreaded(false);
    }
    const Uint64 t1 = SDL_GetPerformanceCounter();
    render();
    const Uint64 t2 = SDL_GetPerformanceCounter();
    video.present();
    const Uint64 t3 = SDL_GetPerformanceCounter();

    st.passes++;
    if (vb > 1) st.over++;
    const double work = ms(t0, t2);
    st.workMs += work;
    st.workMax = std::max(st.workMax, work);
    st.frameMs += ms(t0, t1);
    st.renderMs += ms(t1, t2);
    st.presentMs += ms(t2, t3);
    if (ms(st.since, t3) >= 5000.0) {
      int underruns = 0, minQueued = 0;
      audio.takeStats(&underruns, &minQueued);
      nx::keepOverclock();  // (the system may have put the CPU clock back)
      const double videoWait = snes ? snes->ppu.takeStallMs() : 0.0;
      const bool videoThread = snes && snes->ppu.threaded();
      const NxPerf& pf = g_nxPerf;
      logf("perf %s, %.1f s: %d passes, %d over a refresh (%.1f%%), %d catch-ups; work %.1f ms avg / %.1f max "
           "(frame %.1f: emulation %.1f per game frame, rewind %.1f per snapshot; render %.1f); present wait %.1f; "
           "sound: %d underruns, queue low %d frames; CPU %u MHz; video thread %s (the emulation waited %.2f ms per pass for it)",
           inLauncher ? "launcher" : "game", ms(st.since, t3) / 1000.0, st.passes, st.over,
           100.0 * st.over / std::max(1, st.passes), st.catchUps, st.workMs / std::max(1, st.passes), st.workMax,
           st.frameMs / std::max(1, st.passes), pf.emuFrames ? pf.emuMs / pf.emuFrames : 0.0,
           pf.rewindPushes ? pf.rewindMs / pf.rewindPushes : 0.0, st.renderMs / std::max(1, st.passes),
           st.presentMs / std::max(1, st.passes), underruns, minQueued, nx::cpuClockHz() / 1000000, videoThread ? "on" : "off",
           videoWait / std::max(1, st.passes));
      g_nxPerf = NxPerf();
      st = Stats();
      st.since = SDL_GetPerformanceCounter();
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
      const std::string why = g_initError.empty() ? SDL_GetError() : g_initError;
      app->shutdown();
      g_app = nullptr;
      app.reset();
      nx::showFatal(("The program could not start (OpenGL ES 3.0 or the sound).\n\n" + why +
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
