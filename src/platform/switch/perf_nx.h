// Per-pass timing for the Switch build's frame-pacing log (see App::run in app_switch.cpp): the
// emulation and the rewind snapshots are timed where they happen, in app/app.cpp.
#pragma once

struct NxPerf {
  double emuMs = 0;      // the game's frames (beforeFrame, runFrame, afterFrame, the sound)
  int emuFrames = 0;
  double rewindMs = 0;   // the rewind snapshots
  int rewindPushes = 0;
};
extern NxPerf g_nxPerf;
