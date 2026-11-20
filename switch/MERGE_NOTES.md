# Merging upstream into the Switch port

This tree is upstream 1.1.1 plus the Switch layer. To take a later upstream version, merge it three ways
(old upstream, new upstream, this tree): the Switch-only files never conflict, and only these shared files
carry Switch edits (all behind `__SWITCH__`):

* `src/app/app.cpp`: no Updates section, no Display Mode / fullscreen key, no on-screen-touch options, no
  keyboard bindings or keyboard hotkeys in the menus (Controls shows "Player 1/2 Controller", the hotkey list
  shows the controller's binding only, no Fullscreen), and the start-up and rebinding hints name the
  controller's buttons; the
  covers, MSU-1 folder and achievements-export items show a path instead of opening a file manager.
* `src/app/config.h`, `config.cpp`, `app.cpp`: also the Switch's CPU clock setting (`cpuClock`, `[switch]` in
  the ini) and its item under Options > Performance.
* `src/app/config.cpp`, `config.h`: the pad button names are the Switch's; menus confirm with the right
  button (its A) by default (`confirmRight`).
* `src/app/launcher.cpp`: "Switch Edition" and the ROM-not-found message.
* `src/app/updatecheck.h`, `updatecheck.cpp`: a do-nothing UpdateCheck (no network, no `std::thread`).
* `src/core/fileio.cpp`: the SD card cannot rename over an existing file, so the old one goes first.
* `src/platform/android/video_gles.cpp`, `video_gles.h`: the Switch's context (`egl_nx.cpp`), glyphs
  (`glyphs_nx.cpp`) and the log in `smas_port.log`; the Android and Linux paths are untouched.

* `src/core/rewind.cpp`, `rewind.h`, `snes.cpp`: speed only. `Rewind::encode` scans eight bytes at a time
  and writes exactly the bytes the old byte-by-byte scan did (`encodeSlow`, kept, is used if the sizes
  differ); `stateCrc` uses the ARM CRC-32 instructions under `__ARM_FEATURE_CRC32` (same checksum);
  `saveState` reserves its buffer.
* `src/core/ppu.cpp`, `ppu.h`: speed only, and the biggest part of it (the picture is about three quarters of
  the emulation's time). `renderBgFast` draws a background a tile row at a time (the general per-pixel
  `renderBg` stays, for mosaic and the offset-per-tile modes, and is the reference); `composeLine` walks
  a per-line list of the layers that have something on the line and picks the top pixel with a small
  table, without branches. The output, and the calls to the game hooks, are the same as the old code's:
  `tools/perf/ppu_equiv_test.cpp` checks that over thousands of random scenes (see `tools/perf/README.md`),
  so after a merge that touches `ppu.cpp`, run it against upstream's `ppu.cpp` as the reference.
* `src/platform/android/input_sdl.h`: under `__SWITCH__` the app's `PadState` is renamed (a macro) so it
  does not share a name with libnx's `PadState` (an ODR violation that link-time optimisation reports).
* `src/core/ppu.cpp`, `ppu.h`, `snes.cpp`, and the new `src/core/jobqueue.h`, `jobqueue_std.h`: the final mix of
  each line (`composeCore`) can run on a worker thread from a copy of the line's layers (`composeLine`'s
  `async` path); with no `Ppu::queueFactory` set, or `setThreaded(false)`, it runs in place exactly as before.
  `Snes::runFrame` ends with `ppu.waitRender()`. The Switch supplies the queue (`platform/switch/jobqueue_nx.cpp`)
  and turns it on for the game's Ppu only (`app.cpp`: `loadRom`, `applyConfig`).
* `src/platform/android/audio_sdl.cpp`: a `reserve` in the resampling loop. `video_gles.cpp`: the UI
  vertex buffer is uploaded at its real size on the Switch. `src/app/app.cpp`: the emulation and rewind
  are timed for the log under `__SWITCH__` (`platform/switch/perf_nx.h`).

The Switch-only code is `src/platform/switch/` and this folder. The platform functions upstream's `sys.h`
declares are in `app_switch.cpp` (`httpGet` and `openFolder` do nothing here) and `app/sys_posix.cpp`.
