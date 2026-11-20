# Super Mario All-Stars - Nintendo Switch (homebrew .nro)

A Switch build of the Android/PC port (upstream version 1.1.1): the same emulation core, game modules,
launcher, menus and widescreen code, with a Switch platform layer (`src/platform/switch/`). It contains
no game data: it runs from your own ROM.

Everything in upstream 1.1 and 1.1.1 that makes sense on a console is in: the gameplay, audio and
widescreen/co-op fixes, MSU-1 custom soundtracks (in the games and the Music Player), the faster and
smoother launcher, your own game covers and launcher background, the four menu styles, Music Volume and
Sound Effects Volume (Options > Audio), the NES Brick Bounce option, the Menu Buttons swap and the
achievements export.

## Build
1. Install [devkitPro](https://devkitpro.org/wiki/Getting_Started) and, with `dkp-pacman`:
   ```
   sudo dkp-pacman -S switch-dev switch-sdl2 switch-mesa switch-freetype
   ```
2. ```
   export DEVKITPRO=/opt/devkitpro
   cd switch
   make
   ```
   The result is `switch/SuperMarioAllStars.nro`.

## Install
On the SD card, make a folder and put the `.nro` and your ROM in it, side by side:
```
sdmc:/switch/SuperMarioAllStars/SuperMarioAllStars.nro
sdmc:/switch/SuperMarioAllStars/Super Mario All-Stars (USA).sfc      (.sfc / .smc; a "roms" subfolder works too)
```
Start it from the Homebrew Menu. Settings, battery saves, save states, screenshots and the log
(`smas_port.log`) are written to that same folder. The program also makes these folders beside the
`.nro` for you to fill from a computer: `roms`, `covers` and `msu`. If the ROM is missing, the launcher card says so
and picks it up within a second of it being copied in.

## Controls
Buttons are by position, like the SNES pad: the Switch's B (bottom) is the SNES B, A (right) is A,
Y (left) is Y, X (top) is X; L/R, ZL = rewind, ZR = fast forward, + = Start, - = menu.
The game's own Select is a left-stick press (or Menu > Press Select In Game). In the menus and the
launcher A confirms and B goes back. The touch screen taps and scrolls the launcher and menus.
Everything can be rebound under Menu > Controls. Controllers 1 to 8 are read (the handheld pad counts
as controller 1); two-player co-op needs the second player on another controller.

## Performance
The aim is a steady 60 fps. What the build does for it:

* The pacing assumes the Switch's fixed 60 Hz screen (it used to measure it, and a few slow passes made it
  read 48 Hz and flip to an uneven mode: the speed-ups and slow-downs). A pass that runs over a refresh is
  not followed by a two-frame catch-up pass unless the sound is running short, so one hitch cannot leave
  the picture at 30 fps.
* The picture (about three quarters of the emulation's time) is drawn about twice as fast as before: the
  backgrounds a tile row at a time, and the final mix of the layers without per-pixel branching. The result
  is the same pixel for pixel (a test renders thousands of random scenes both ways: `tools/perf/`).
* The picture's last step, mixing each line's layers into the final colours (a third of the emulation's time),
  runs on a second CPU core: the emulation hands each finished line to a worker thread (core 1, else 2) and
  carries on, and the picture is complete when the frame ends. It is on by default and has no menu entry; to turn it off
  set `threaded_video=0` under `[switch]` in `smas_port.ini`. The log says which
  core it started on, and the `perf` line says how long the emulation waited for
  it. (The sound emulation is only a few percent of the work, so it is left alone; the rest of the picture,
  the backgrounds and sprites, reads the game's memory as it runs and stays with the game.)
* The rewind snapshots (every second frame) are much cheaper: the state's checksum uses the processor's
  CRC-32 instructions and the difference is found eight bytes at a time.
* The UI is uploaded to the GPU at its real size, not as a fresh 2.6 MB buffer for every flush.
* The program is built with link-time optimisation (the CPU core reads memory through another file, and
  the compiler can now inline that). If your toolchain has no LTO plugin: `make clean && make LTO=0`.
  Changing that setting needs a `make clean` first.
* The CPU clock is the system's own unless you choose a higher one (Stock by default; there is no menu entry, so set
  `cpu_clock=` under `[switch]` in `smas_port.ini`: 0 stock, 1 1224 MHz, 2 1581 MHz, 3 1785 MHz).

Every five seconds the log (`smas_port.log`) gets a `perf` line, for the game or the launcher:
`perf game, 5.0 s: 300 passes, 4 over a refresh (1.3%), 0 catch-ups; work 11.2 ms avg / 17.9 max (frame 9.4:
emulation 8.1 per game frame, rewind 0.7 per snapshot; render 1.8); present wait 0.5; sound: 0 underruns,
queue low 2300 frames; CPU 1785 MHz; video thread on (the emulation waited 0.01 ms per pass for it)`. A pass has 16.7 ms. The number to watch is `emulation` per game
frame: above about 15 ms the game cannot hold 60 fps. (`render` also contains the wait for the screen's
next refresh whenever the emulation was quick, so a render of several ms is spare time, not work.) `CPU` is
the clock actually read back; it should say 1785 MHz. Options >
Display > Show FPS shows the game's own frame rate on screen. If it is still short, Options > Rewind off
saves the snapshot work.

## Custom soundtracks (MSU-1)
Put each music pack in its own folder inside `msu` (next to the `.nro`): the MSU-1 patch (`.bps` or
`.ips`, for the USA ROM) and its `NAME-N.pcm` tracks (and `NAME.msu` if it has one). Then choose the
pack under Options > Audio. The patch is applied to the ROM in memory as the game loads; your ROM file is
never changed. A pack can be switched back off the same way. The tracks are read from the SD card as they
play. (Options > Audio > MSU-1 Folder shows where the folder is.)

## Covers and background
PNG or JPG pictures in the `covers` folder: `smb1`, `lostlevels`, `smb2`, `smb3`, `allstars` (the single
card) and `background`. Then switch on Game Covers / Custom Background in the Options menu. Pictures are read when you
switch those on.

## Notes
* There is no update check and no web links on the Switch build (no network access): a new version is a
  new `.nro`.
* Achievements can be exported with Export to CSV on the Achievements page; the files (`achievements.csv` and an `icons`
  folder) go to a folder beside the `.nro`, whose path the menu shows.
* The CPU runs at the system's own clock (stock, 1020 MHz) unless you raise it: Options > Performance > CPU
  Clock cycles Stock, 1224, 1581 and 1785 MHz, and the choice is kept in `smas_port.ini`. A higher clock
  keeps heavy scenes at 60 fps and costs battery and heat; the log says what clock was actually reached.
* There is no keyboard on the Switch, so the menus have none of its settings: Options > Controls lists the
  two players' controller bindings only, and the hotkey list shows each action's controller button (or
  "unbound": Save state, Load state and Screenshot can be bound to a controller button there).
* Controllers: Switch buttons are read by position, the same as the SNES pad (the Switch's B, the bottom
  button, is the SNES B). A single Joy-Con is read as held sideways, with its SL and SR as L and R. The
  first button presses on controllers 1 and 2 are written to the log with the controller's kind, which
  shows what the system reports if a button does not do what it should.
* Text uses the system's shared font through FreeType; the title font (Luckiest Guy, OFL) is in the
  .nro's romfs. Symbols the system font lacks are drawn as plain look-alikes.
* The picture is drawn at 1280x720 (the system's own layer size); docked, the system scales it to the
  screen. (Options > Display still sets how the game picture fills it.)
* VSync (Options > Display, or `vsync=` under `[display]` in smas_port.ini) is the player's setting:
  on, each frame waits for the screen's refresh; off, the swap interval is 0 and the loop is timed
  to the game's own 60.1 fps instead (a little less input lag, can tear). It changes live.
* Cheats > Save Level Progress and More Active Fireballs: see `src/game/progress.h`,
  `src/game/smb1fire.cpp` and `src/game/smb3fire.cpp`. The log says "Level progress saved" /
  "restored" when they act.
* Not available here: the Windows-only window, black-frame and slang-shader options, and the
  Android touch pad.
* The video layer is the Android one (OpenGL ES 3.0), but its context is made directly on the console's
  native window with devkitPro's Mesa EGL (`platform/switch/egl_nx.cpp`), the way the devkitPro OpenGL
  examples do, with no SDL window or video driver in between. SDL2 is only used for the sound and the
  clock; the touch screen and the HOME-menu focus come from libnx. If the program closes at start,
  `smas_port.log` says why.

## If the screen stays black
Every start writes `smas_port.log` next to the .nro. Open it after a run: it records each step
(EGL version and the configuration chosen, the surface size, the OpenGL vendor/renderer/version, how long
each shader took to build, GL errors, and `frame N presented` lines for the first frames). The last line
shows how far the program got:

* `EGL:` lines with no `EGL surface` line: the context could not start (no display, no OpenGL ES 3
  configuration, context or surface; the message has EGL's own error code). The console then shows
  the reason.
* `GL error:` lines: a shader did not build. The picture shader then falls back to a plainer version
  (no CRT, LCD, colour-vision or blurred-side passes) and the log says so.
* `frame 1 presented` but the screen is black: the `EGL surface` line, the `GL version` line and the
  `glGetError` lines say whether the surface or the driver is at fault.
* `main loop starts` but no `frame` lines: the first pass is stuck before it draws.
* `program N linked ... isProgram 0`, `the picture program is not usable` or uniforms of `-1` with a
  `glGetError 0x0501`: the driver handed back a shader program it then treats as invalid, so nothing can
  be drawn. The video setup is then tried again (up to three tries, from a new context, the later ones
  leaving the shader objects alone), and each try is logged.

The `native window set` line is the important one for a black screen: the native window has to be given
its size before the surface is made, or Mesa has nothing to show.

Please keep `smas_port.log` when reporting a problem.
