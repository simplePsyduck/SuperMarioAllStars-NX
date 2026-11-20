#include "game/smb3.h"
#include "game/stateext.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <vector>

namespace {
constexpr int kHudRow = 192;       // status bar split (IRQ at V=192)
constexpr int kIrisX = 128, kIrisY = 127;  // centre of the level-opening circle
constexpr int kLevelHeight = 432;  // 27 metatile rows
// Where the 4:3 game spawns objects ahead: its spawn column is (camera +
// $110) & ~15, so an object in 16-px column c is reached once c - camera <=
// $110.
constexpr int kSpawnReach43R = 0x110;
constexpr int kCruise = 8;  // the usual auto-scroll speed (1/2 px a frame)
// Beyond either 4:3 spawn column (left: (camera - $20) & ~15), for an object
// in column `rel` (relative to the camera).
bool beyondReach43(int rel) { return rel > kSpawnReach43R || rel < -0x2f; }
// The object update loop's JSR $90EC and the instruction after it (see the
// hooks further down).
constexpr uint32_t kObjUpdate = 0x27906a, kObjUpdated = 0x27906d;
// (... and the slot's turn over: past its tail check, JSR $A5BB)
constexpr uint32_t kObjDone = 0x279070;
// The spawn routine ($29:9AE0) has matched level object X to the column it
// checks; its loop on to the next level object (see holdControl).
constexpr uint32_t kSpawnMatch = 0x299b37, kSpawnNext = 0x9b0e;
// An object's start as its initialisation leaves it (see kObjUpdate): X lo/hi,
// Y lo/hi, X speed, Y speed, X fraction. (Restoring more of its variables
// made some enemies' first moves differ, as their AI reads shared state.)
constexpr uint16_t kObjVars[] = {0x005f, 0x0044, 0x0071, 0x0056, 0x008c, 0x009e, 0x074f};
constexpr int kObjVarCount = sizeof(kObjVars) / sizeof(kObjVars[0]);
static_assert(kObjVarCount == 7, "Smb3Engine::Hold::vars size");
// World-map panel metatiles: level N = $02+N, fortress $67.
constexpr uint8_t kPanelFortress = 0x67;
}  // namespace

Smb3Engine::Smb3Engine(Snes& snes) : snes_(snes) {
  // Until measured: hills at half speed, clouds at a quarter (most levels).
  for (auto& k : knownSpeed_) {
    k[0] = 1.0;
    k[1] = 0.5;
    k[2] = 0.25;
  }
}

void Smb3Engine::patch(uint32_t adr, uint8_t v) {
  if (!original_.count(adr)) original_[adr] = snes_.peek(adr);
  snes_.poke(adr, v);
}

void Smb3Engine::unpatchAll() {
  for (auto& o : original_) snes_.poke(o.first, o.second);
}

void Smb3Engine::frameStart(int want, bool active, bool hudAnchor) {
  hudAnchor_ = hudAnchor;
  want_ = want;
  active_ = active;
  uint8_t ts = ram(0x070a), nmi = ram(0x0100);
  vertical_ = ram(0x0425) != 0;
  // Levels: tilesets 1-14 (7: the toad house, a single-page room), in the
  // level NMI modes ($A0: levels with an extra raster split, e.g. 3-2 and the
  // tank levels).
  bool levelTs = ts >= 1 && ts <= 0x0e;
  bool levelNmi = nmi == 0xc0 || nmi == 0x80 || nmi == 0x95 || nmi == 0xa0;
  // A level fades in (and waits a moment before play) in NMI mode $00, after
  // being built while the screen is black, and fades out in it: keep the
  // level's view through those frames, so it doesn't change at full
  // brightness, until the screen has gone dark after being lit.
  bool black = snes_.ppu.forcedBlank() || snes_.ppu.brightness() == 0;
  if (levelTs && levelNmi) {
    levelShown_ = true;
    levelLit_ = false;
  } else if (!levelTs || nmi != 0x00 || (black && levelLit_)) {
    levelShown_ = false;
  } else if (!black) {
    levelLit_ = true;
  }
  gameplay_ = levelTs && (levelNmi || levelShown_);
  cam16_ = ram(0x0210) | (ram(0x0211) << 8);
  {
    // The auto-scroll speed picking up (see framesToSpawn).
    int speed = (int8_t)ram(0x1a0e);
    if (speed > lastSpeed_ && speed < kCruise) rampFrames_ = 8;
    else if (rampFrames_ > 0 && speed != lastSpeed_ + 1 && speed != lastSpeed_) rampFrames_ = 0;
    else if (rampFrames_ > 0 && speed >= kCruise) rampFrames_ = 0;
    else if (rampFrames_ > 0 && speed == lastSpeed_) rampFrames_--;
    ramping_ = rampFrames_ > 0;
    lastSpeed_ = speed;
  }
  // Single-page areas (king's rooms, bonus rooms, boss rooms) have nothing
  // beside the 256 px screen: they are shown wide, centred, black either
  // side (see beginLine). Their objects keep the 4:3 game's range (an object
  // listed past the end of one appeared in World 6).
  onePage_ = !vertical_ && ram(0x0022) == 0;
  setPiece_ = onePage_ && ram(0x070a) != 0x0a;
  // The Spade bonus game (lining up the picture's three strips; tileset $10,
  // NMI mode $40) is shown wide too: its framed board centred, black either
  // side, the status bar centred (see beginLine).
  spade_ = ts == 0x10 && nmi == 0x40;
  ext_ = (gameplay_ || spade_) && want > 0 ? std::min(want, 112) : 0;
  currentSplit(&extL_, &extR_);
  // The circle a level opens with (centred on the game's screen; its radius
  // is $0281 - 10 on the next frame) stops once it covers the game's 256 px:
  // carry it on over the margins until it covers them too.
  // (Some levels, 3-5, run it in the play mode rather than the fade-in one.
  // The pause menu uses $0281 (its box's size) and the window too: the
  // circle is only taken up from its start, $0281 = 8.)
  if (ram(0x037a) != 0) {
    irisR_ = -1;
  } else if (gameplay_ && ram(0x0281) != 0 && snes_.ppu.mainScreenWindow() != 0 &&
             (irisR_ >= 0 || ram(0x0281) <= 0x10)) {
    irisR_ = ram(0x0281) - 10;
  } else if (irisR_ >= 0 && gameplay_) {
    irisR_ += 8;
    int dx = std::max(kIrisX + extL_, 256 + extR_ - kIrisX), dy = std::max(kIrisY, kHudRow - kIrisY);
    if (irisR_ * irisR_ >= dx * dx + dy * dy) irisR_ = -1;
  } else {
    irisR_ = -1;
  }
  if (gameplay_) {
    // Metatile -> tilemap table for the current tileset.
    uint8_t bank = snes_.peek(0x21ce80 + ts);
    tileTable_ = (uint32_t(bank) << 16) | snes_.peek16(0x21ce5a + 2 * ts);
    memcpy(tileSnap_[0], snes_.wram + 0x2000, kTileMem);
    memcpy(tileSnap_[1], snes_.wram + 0x4000, kTileMem);
  }
  // Object activity range (data tables in bank $21, compare operands in $27).
  if (active && ext_ > 0 && !onePage_ && !spade_) {
    if (adoptPending_ && gameplay_) adoptSweptObjects();
    if (ram(0x05fe) != 0 && !vertical_) {
      // 5-9's camera jumps ahead, carrying what is on screen with it ($05FE,
      // $27:A994): what only the wide view has so far belongs to the part of
      // the level left behind. It goes, and the wide range rests until the
      // jump is over (then catches up with the new part). (Spawning ahead in
      // the wide range during the jump, not carried until the 4:3 game would
      // have it, lost platforms: the jump also wraps the camera vertically,
      // and they were left below the screen and deleted; 5-9 couldn't be
      // finished.)
      for (int s = 0; s <= 4; s++) {
        uint8_t idx = ram(0x0659 + s);
        if (!optional_[s] || !ram(0x0661 + s) || idx >= 0x30) continue;
        snes_.wram[0x0661 + s] = 0;
        snes_.wram[0x1b10 + idx] &= 0x7f;
        optional_[s] = optNormal_[s] = false;
        held_[s] = kFree;
      }
      forgetParked();
      spawn_.inactive();
      patchSpawn(0, 0);
    } else {
      trackSpawnGrowth();
      patchSpawn(extL_, extR_);
    }
    static const int kDel[6] = {0x0120, -0x0030, 0x0180, -0x0080, 0x0140, -0x0050};
    for (int i = 0; i < 6; i++) {
      int v = kDel[i] + (i % 2 == 0 ? extR_ : -extL_);
      patch(0x21afe7 + i, v & 0xff);
      patch(0x21afed + i, (v >> 8) & 0xff);
    }
    patch16(0x27b515, 0x0100 + extR_);                // player fireballs
    patch16(0x27b51a, (0xfff8 - extL_) & 0xffff);
    patch16(0x27c588, 0x00f8 + extR_);                // special objects
    patch16(0x27c58d, (0xfff8 - extL_) & 0xffff);
    patch16(0x27bd39, (0xfff0 - extL_) & 0xffff);     // misc objects
    patch16(0x27bd3e, 0x0100 + extR_);
    patch16(0x2989d7, 0x0100 + extR_);                // burner flames' pieces ($29:89AB): [-32,256)
    patch16(0x2989dc, (0xffe0 - extL_) & 0xffff);
  } else {
    unpatchAll();
    spawn_.inactive();
    forgetParked();  // (parked objects' spawn flags are clear: the 4:3 range reaches them)
    forgetHeldControls();  // (likewise the held controllers)
  }
  updateWarp();
}

bool Smb3Engine::autoscrollBounds() {
  constexpr uint16_t kInside = 0x86e5, kDone = 0x86f6;  // where the game's test continues
  if (!gameplay_ || ext_ == 0 || vertical_) return false;
  CpuRegs& r = snes_.cpu.r;
  if (!r.mf) return false;
  int x = ram(0x005e) | ram(0x0043) << 8, cam = ram(0x0210) | ram(0x0211) << 8;
  // (The view's edges, or the level's ends where the view, centred on the
  // game's screen, shows past them: the player stays inside the level.)
  int levelEnd = (ram(0x0022) + 1) * 256;
  int edgeL = std::max(cam - extL_, 0), edgeR = std::min(cam + 256 + extR_, levelEnd);
  bool left = x - edgeL <= 0x0f, right = x - edgeR >= -0x20;
  uint16_t next = kInside;
  if (left || right) {
    // The game holds the player at its screen's edge to the scroll's speed,
    // so the edge never runs into him. At a level's end the edge stands still
    // before the game's camera does: there it stands still, and so must he.
    // At the scroll's speed he drifted on into it and died (World 3's cloud
    // treasure area at 21:9, holding right).
    bool pinned = right ? cam + 256 + extR_ >= levelEnd : cam - extL_ <= 0;
    uint8_t v = pinned ? 0 : ram(0x1a0e), m = ram(0x008b);
    if (!((v ^ m) & 0x80)) snes_.wram[0x074e] = 0;  // x fraction
    uint8_t a = (uint8_t)(v - m - 1);
    if (right) a ^= 0x80;  // (the game: EOR $79, whose bit 7 tells the edges apart)
    if (!(a & 0x80)) snes_.wram[0x008b] = v;
    next = kDone;
  }
  r.pc = next;
  return true;
}

bool Smb3Engine::edgeDeath() {
  constexpr uint16_t kDie = 0xc32a, kLive = 0xc33d;
  if (!gameplay_ || ext_ == 0 || vertical_) return false;
  CpuRegs& r = snes_.cpu.r;
  if (!r.mf) return false;
  int x = ram(0x005e) | ram(0x0043) << 8, cam = ram(0x0210) | ram(0x0211) << 8;
  // (The game: $79 >= $F8, i.e. 8 px past either side of its screen; pinned
  // against a wall by the scroll, the player is carried off the left side.)
  int levelEnd = (ram(0x0022) + 1) * 256;
  int edgeL = std::max(cam - extL_, 0), edgeR = std::min(cam + 256 + extR_, levelEnd);  // (see autoscrollBounds)
  bool past = x >= edgeR - 8 || x < edgeL;
  r.a = (r.a & 0xff00) | ram(0x0079);  // (as LDA $79 / CMP would leave them)
  r.c = past;
  r.pc = past ? kDie : kLive;
  return true;
}

bool Smb3Engine::shellBump(bool edge) {
  if (!gameplay_ || ext_ == 0 || vertical_) return false;
  CpuRegs& r = snes_.cpu.r;
  if (!r.mf) return false;
  const int s = r.x & 0xff;
  if (s > 7) return false;
  const int rel = (ram(0x005f + s) | ram(0x0044 + s) << 8) - cam16_;
  const bool right = !(ram(0x008c + s) & 0x80);
  if (edge) {
    // (LDA $7A,X: the screen edge test on the view's edges instead)
    const bool atEdge = right ? rel >= 256 + extR_ - 28 : rel < 6 - extL_;
    const uint8_t v = atEdge ? (right ? 0xff : 0x00) : 0x80;
    r.a = (r.a & 0xff00) | v;
    r.n = v & 0x80;
    r.z = v == 0;
    r.pc += 3;
    return true;
  }
  // (JSL $27:A85F, the off-screen bits: in the view, on)
  if (rel + 16 <= -extL_ || rel >= 256 + extR_) return false;
  r.a &= 0xff00;
  r.z = true;
  r.n = false;
  r.pc += 4;
  return true;
}

// The walk off after the goal: in the wide view the player walks on past the
// 4:3 screen's edge to the view's (the level carries on there), so his sprites
// are placed at their true X, from his 16-bit position, rather than hidden as
// they wrap; only those past the view's edge are hidden.
void Smb3Engine::goalWalk() {
  std::memset(goalShown_, 0, sizeof goalShown_);
  if (!gameplay_ || ext_ == 0) return;
  int cam = ram(0x0210) | ram(0x0211) << 8;
  int rel = (ram(0x0043) << 8 | ram(0x005e)) - cam;
  int at = ram(0x00c4) | ram(0x00c5) << 8;
  for (int k = 0; k < 12; k++, at += 4) {
    if (at > 0x1fc) break;
    int i = at >> 2;
    if (ram(0x0801 + at) >= 0xe0) continue;  // (not drawn)
    int x = rel + (int8_t)(uint8_t)(ram(0x0800 + at) - (rel & 0xff));
    if (x < 256) continue;
    if (x < 256 + extR_) {
      oamX_[i] = (int16_t)x;
      oamXSet_[i] = goalShown_[i] = true;
    } else {
      snes_.wram[0x0801 + at] = 0xf0;
    }
  }
}

// ... and he stops 32 px past the view's edge instead, where the tally then
// starts (`on`: the test before the tally; else the one stopping him, where
// Y is $FF).
bool Smb3Engine::goalStop(bool on) {
  if (!gameplay_ || ext_ == 0) return false;
  int cam = ram(0x0210) | ram(0x0211) << 8;
  int rel = (ram(0x0043) << 8 | ram(0x005e)) - cam;
  bool past = rel >= 256 + extR_ + 0x20;
  if (on) {
    snes_.cpu.r.pc = past ? 0xcb36 : 0xcb63;  // ($28:CB36 on; $CB63 not yet)
  } else {
    if (past) snes_.wram[0x0556] = 0xff;
    snes_.cpu.r.pc = 0xcaef;  // (its RTS, $28:CAEF)
  }
  return true;
}

// Objects the level-load sweep spawned beyond the 4:3 reach appeared before
// play started (holding off): take them on from the first frame of play.
void Smb3Engine::adoptSweptObjects() {
  adoptPending_ = false;
  if (vertical_) return;
  uint8_t* w = snes_.wram;
  int cam = ram(0x0210) | ram(0x0211) << 8;
  for (int slot = 0; slot <= 4; slot++) {
    uint8_t st = w[0x0661 + slot];
    prevState_[slot] = st;
    if (!st) continue;
    int col = ((w[0x0044 + slot] << 8) | w[0x005f + slot]) & ~15;
    held_[slot] = (int16_t)(col - cam) > kSpawnReach43R ? kWaiting : kFree;
    if (!held_[slot]) continue;
    hold_[slot].col = (int16_t)col;
    returning_[slot] = false;
    holdCaptured_[slot] = st != 1;  // (still to initialise: taken on after that)
    if (!holdCaptured_[slot]) continue;
    captureHold(slot);
    leadIn(slot);
    prevState_[slot] = w[0x0661 + slot];
  }
  holdInit_ = true;
}

void Smb3Engine::captureHold(int slot) {
  Hold& h = hold_[slot];
  h.state = snes_.wram[0x0661 + slot];
  for (int i = 0; i < kObjVarCount; i++) h.vars[i] = snes_.wram[kObjVars[i] + slot];
}

// Frames until the 4:3 game spawns an object in column `col`: when its camera
// reaches col - $110. 0: this frame (or past); -1: not while the scroll stands.
int Smb3Engine::framesToSpawn(int col) const {
  int cam = ram(0x0210) | ram(0x0211) << 8;
  long pos = cam * 256L + ram(0x1a10);  // the auto-scroll camera, in 1/256 px
  long target = (col - kSpawnReach43R) * 256L;
  if (pos >= target) return 0;
  int speed = (int8_t)ram(0x1a0e);  // its speed (4.4 px per frame)
  if (speed <= 0) return -1;
  if (ramping_) {
    // The scroll picks up at a level start, 1/16 px per frame more every
    // fourth frame, to its usual 1/2 px.
    int k = 0;
    for (int sub = ram(0x1a09); pos < target && k < 4096; k++) {
      pos += speed * 16;
      if (--sub <= 0) {
        sub = 4;
        if (speed < kCruise) speed++;
      }
    }
    return k;
  }
  return (int)((target - pos + speed * 16 - 1) / (speed * 16));
}

// An object waiting beyond the view on the 4:3 timeline would hold one of the
// five level-object slots all that time, so objects the 4:3 game has could
// find none free (5-6 lost most of its Para-Beetles at 21:9): remove it, with
// its level object spawnable again, and spawn it once it is due in view.
bool Smb3Engine::park(int slot) {
  uint8_t idx = ram(0x0659 + slot);
  if (parkedCount_ == kMaxParked || idx >= 0x30) return false;  // (flags: $1B10-$1B3F)
  parked_[parkedCount_++] = Parked{idx, hold_[slot]};
  snes_.wram[0x0661 + slot] = 0;
  snes_.wram[0x1b10 + idx] &= 0x7f;
  held_[slot] = kFree;
  return true;
}

// The column this frame's spawn check should look at instead of its own, if
// any (and a slot is free): a parked object whose place on the 4:3 timeline
// has come into view, or a spawn that found no free slot, still in range.
bool Smb3Engine::dueColumn(int* col) {
  int cam = cam16_ & 0x8000 ? cam16_ - 0x10000 : cam16_;
  int freeSlots = 0;
  for (int s = 0; s <= 4; s++) freeSlots += ram(0x0661 + s) == 0;
  auto at = [&](const Hold& h, int k) {
    return ((h.vars[1] << 16 | h.vars[0] << 8 | h.vars[6]) - std::max(k, 0) * (int8_t)h.vars[4] * 16) >> 8;
  };
  for (int i = 0; i < parkedCount_; i++) {  // (spawned meanwhile, or gone past)
    const Hold& h = parked_[i].start;
    if ((ram(0x1b10 + parked_[i].idx) & 0x80) || at(h, framesToSpawn(h.col)) - cam < -extL_ - 64)
      parked_[i--] = parked_[--parkedCount_];
  }
  for (int i = 0; i < retryCount_; i++) {  // (spawned, out of range, or tried enough)
    // Also one well inside the 4:3 game's screen by now: had that game had
    // room for it, so would this one (see evictOptional); it lost it too.
    int rel = retry_[i].col - cam;
    if (rel > kSpawnReach43R + extR_ || rel < -0x30 - extL_ || (ram(0x1b10 + retry_[i].idx) & 0x80) ||
        retry_[i].tries >= 8 || (rel > -0x10 && rel < 0xe0))
      retry_[i--] = retry_[--retryCount_];
  }
  // First what the 4:3 game has by now (its spawn point is reached): room is
  // made for it if need be, from an object only the margins show so far.
  auto take = [&](int c) {
    *col = c;
    return true;
  };
  for (int i = 0; i < parkedCount_; i++)
    if (framesToSpawn(parked_[i].start.col) == 0 && (freeSlots > 0 || evictOptional()))
      return take(parked_[i].start.col);
  for (int i = 0; i < retryCount_; i++)
    if (!beyondReach43(retry_[i].col - cam) && (freeSlots > 0 || evictOptional())) {
      retry_[i].tries++;
      return take(retry_[i].col);
    }
  // Then objects for the margins, the one nearest to coming into view first.
  // While it is out of sight a slot is kept free for the above. About to come
  // into view (it would pop in), it takes the last free slot, or one from an
  // object the 4:3 game has deleted by now, or one out of sight further out
  // (see evictHidden). Objects the wide view kept behind the player held all
  // five slots, so those ahead only appeared at their 4:3 spawn point, well
  // inside the view.
  int best = -1, bestOut = 0;
  bool bestParked = false;
  for (int i = 0; i < parkedCount_; i++) {
    const Hold& h = parked_[i].start;
    int k = framesToSpawn(h.col);
    int out = k > 0 ? outside(at(h, k) - cam) : 0;
    if (k > 0 && out <= 16 && (best < 0 || out < bestOut)) {
      best = i;
      bestOut = out;
      bestParked = true;
    }
  }
  for (int i = 0; i < retryCount_; i++) {
    int out = outside(retry_[i].col - cam);
    if (best < 0 || out < bestOut) {
      best = i;
      bestOut = out;
      bestParked = false;
    }
  }
  if (best < 0) return false;
  if (freeSlots < 2 && !(bestOut <= 16 && (freeSlots == 1 || evictHidden(bestOut)))) return false;
  if (bestParked) return take(parked_[best].start.col);
  retry_[best].tries++;
  return take(retry_[best].col);
}

// How far an object at `vx` (relative to the camera, 16 px wide) is outside
// the view; < 0: in it.
int Smb3Engine::outside(int vx) const { return std::max(-extL_ - 16 - vx, vx - 256 - extR_); }

// Off-screen and beyond the 4:3 game's delete range for object `s` (the one
// it was last tested with, see kDeleteRange; the widest until then): the 4:3
// game has deleted it by now, the wide range keeps it for the margins.
bool Smb3Engine::gone43(int s, int vx) const {
  static const int kDelL[3] = {-0x30, -0x80, -0x50}, kDelR[3] = {0x120, 0x180, 0x140};
  int range = deleteRange_[s] == 0 || deleteRange_[s] == 2 || deleteRange_[s] == 4 ? deleteRange_[s] / 2 : 1;
  return ram(0x0661 + s) == 2 && ram(0x0659 + s) < 0x30 && (vx < kDelL[range] - 16 || vx > kDelR[range] + 16);
}

// Frees object slot `s`: one the 4:3 game has deleted by now is removed (its
// level object spawnable again, as there); one only the wide view has so far
// is parked on auto-scroll (back on the 4:3 timeline later), and with normal
// scrolling removed and spawned again when there is room.
bool Smb3Engine::evict(int s, bool gone) {
  uint8_t idx = ram(0x0659 + s);
  if (gone && !optional_[s]) {
    snes_.wram[0x0661 + s] = 0;
    snes_.wram[0x1b10 + idx] &= 0x7f;
    return true;
  }
  optional_[s] = false;
  if (!optNormal_[s]) return park(s);
  optNormal_[s] = false;
  if (idx >= 0x30) return false;
  snes_.wram[0x0661 + s] = 0;
  snes_.wram[0x1b10 + idx] &= 0x7f;
  addRetry(idx, hold_[s].col);
  return true;
}

// Frees a slot for an object only the wide view has, about to come into view
// (`need`: how far it is outside the view), from one out of sight that the
// 4:3 game has deleted by now, or failing that one only the wide view has
// that is further out. Nothing in sight goes: freeing slots from objects the
// 4:3 game has deleted but the margin behind still shows cut the pop-ins
// ahead further, but for more objects vanishing there.
bool Smb3Engine::evictHidden(int need) {
  int cam = cam16_ & 0x8000 ? cam16_ - 0x10000 : cam16_;
  int px = ram(0x005e) | ram(0x0043) << 8, best = -1, bestScore = 0;
  bool bestGone = false;
  for (int s = 0; s <= 4; s++) {
    if (!ram(0x0661 + s)) continue;
    int x = ram(0x005f + s) | ram(0x0044 + s) << 8;
    if (std::abs(x - px) < 48) continue;
    int vx = x - cam, out = outside(vx);
    if (out < 0 && !(ram(0x0681 + s) & 1)) continue;  // (in sight)
    bool gone = gone43(s, vx);
    if (!gone && !(optional_[s] && out > need)) continue;
    int score = (gone ? 0x20000 : 0x10000) + std::max(out, 0);
    if (score > bestScore || best < 0) {
      bestScore = score;
      best = s;
      bestGone = gone;
    }
  }
  return best >= 0 && evict(best, bestGone);
}

// Frees a slot for an object the 4:3 game has by now, from one only the wide
// view has: one out of sight if there is one, else the one the 4:3 game is
// furthest from spawning (never one near the player: he may be standing on
// it). On auto-scroll it is parked (back on the 4:3 timeline later); with
// normal scrolling it is removed and spawned again when there is room.
bool Smb3Engine::evictOptional() {
  int cam = cam16_ & 0x8000 ? cam16_ - 0x10000 : cam16_;
  int px = ram(0x005e) | ram(0x0043) << 8, best = -1, bestScore = 0;
  bool bestGone = false;
  for (int s = 0; s <= 4; s++) {
    if (!ram(0x0661 + s)) continue;
    int x = ram(0x005f + s) | ram(0x0044 + s) << 8;
    if (std::abs(x - px) < 48) continue;
    int vx = x - cam;
    bool gone = gone43(s, vx);  // (removed)
    if (!optional_[s] && !gone) continue;
    bool seen = vx > -extL_ - 16 && vx < 256 + extR_ && !(ram(0x0681 + s) & 1);
    int score = (seen ? 1 : 0x10000) + (gone ? 0x8000 : 0) +
                std::abs(gone || optNormal_[s] ? x - px : hold_[s].col - cam);
    if (score > bestScore) {
      bestScore = score;
      best = s;
      bestGone = gone && !optional_[s];
    }
  }
  return best >= 0 && evict(best, bestGone);
}

// $29:9BC9: the spawn routine found no free slot for level object $01, in
// the column at $05/$06; the 4:3 game would never try it again.
void Smb3Engine::spawnFailed() {
  if (!active_ || want_ <= 0 || vertical_) return;
  addRetry(ram(0x0001), ram(0x0006) << 8 | ram(0x0005));
}

// $29:9C3F: the spawn routine spawned level object $01 from the column at
// $05/$06. It spawns one object per check: another in the same column waits
// for the next check of it, which the 4:3 game makes on the following frames
// as its camera crosses the column, but a column the wide range reaches at
// once (the level-load sweep, a catch-up) is checked only once.
void Smb3Engine::spawnedOne() {
  if (!active_ || want_ <= 0 || vertical_) return;
  int page = ram(0x0006), colByte = ram(0x0005) & 0xf0, done = ram(0x0001);
  if (page > 15) return;
  for (int i = ram(0x1b00 + page); i < 0x30; i++) {
    uint8_t id = ram(0x1b41 + 3 * i), pos = ram(0x1b42 + 3 * i);
    if (id == 0xff || (pos >> 4) != page) break;
    if (i != done && (pos & 15) == colByte >> 4 && id < 0xb4 && !(ram(0x1b10 + i) & 0x80))
      addRetry(i, page << 8 | colByte);
  }
}

// $29:9B37: the spawn routine has matched level object X (id at $1B3F+Y) to
// the column it checks ($05/$06). The level's controllers act the moment they
// spawn rather than as objects: $B4-$BB set a level mode ($0566), $D1-$D6
// start and steer auto-scrolling and the like. The widened range set them off
// a margin's width early: a World 3 cloud area's auto-scroll stopped ~200 px
// short, its chest left beyond reach. They wait for the 4:3 game's own spawn
// columns (left unmarked, they are found again there); enemies and cannons
// keep the wide range.
namespace {
bool isControl(uint8_t id) { return (id >= 0xb4 && id <= 0xbb) || (id >= 0xd1 && id <= 0xd6); }
}  // namespace

bool Smb3Engine::holdControl() {
  if (!active_ || want_ <= 0 || vertical_) return false;
  auto& r = snes_.cpu.r;
  uint8_t id = ram(0x1b3f + (r.y & 0xff)), idx = r.x & 0xff;
  if (!isControl(id) || idx >= 0x30) return false;
  int col = ram(0x0006) << 8 | ram(0x0005);
  int cam = sweeping_ ? sweepCam_ : ram(0x0210) | ram(0x0012) << 8;
  if (cam & 0x8000) cam -= 0x10000;
  if (col <= ((cam + kSpawnReach43R) & ~15) && col >= ((cam - 0x20) & ~15)) return false;
  heldControl_[idx] = true;
  r.pc = kSpawnNext;
  return true;
}

// The column of a controller due now as the 4:3 game would have it: this
// frame's check goes there instead (the game only checks the wide range's
// edge column). $D1-$D6 mark themselves spawned: one held back acts once the
// 4:3 game's spawn columns have reached it. $B4-$BB never do, and the game
// sets their mode again on every frame its column is on them (8 frames as
// the camera runs past): so are they.
bool Smb3Engine::dueControl(int* col) {
  int cam = ram(0x0210) | ram(0x0012) << 8;
  if (cam & 0x8000) cam -= 0x10000;
  int r43 = (cam + kSpawnReach43R) & ~15, l43 = (cam - 0x20) & ~15;
  int col43 = (ram(0x0025) & 1) ? l43 : r43;  // the column the 4:3 game checks this frame
  for (int i = 0; i < 0x30; i++) {
    uint8_t id = ram(0x1b41 + 3 * i), pos = ram(0x1b42 + 3 * i);
    if (id == 0xff) break;
    int c = (pos >> 4) << 8 | (pos & 15) << 4;
    if (id >= 0xb4 && id <= 0xbb) {
      if (c == col43) {
        *col = c;
        return true;
      }
      continue;
    }
    if (!heldControl_[i]) continue;
    if (!isControl(id) || (ram(0x1b10 + i) & 0x80)) {
      heldControl_[i] = false;  // (spawned meanwhile, or another area's list)
      continue;
    }
    if (c > r43 || c < l43) continue;
    heldControl_[i] = false;
    *col = c;
    return true;
  }
  return false;
}

// $27:9C52: the game deletes object X off-screen, leaving its level object
// spawnable again. 5-9 moves every object down 240 px as its camera climbs,
// and deleted the ones the wide range had spawned before that: the 4:3 game
// would only spawn them later, where they belong, so try those again. Only
// those: an object killed, kicked away or fallen into a pit stays gone, as on
// the console (a kicked shell brought 1-4's red Koopa back).
void Smb3Engine::objectDeleted() {
  int slot = snes_.cpu.r.x & 0xff;
  if (!active_ || ext_ == 0 || vertical_ || slot > 4) return;
  int y = ram(0x0056 + slot) << 8 | ram(0x0071 + slot);
  if (prevState_[slot] != 2 || ram(0x0661 + slot) != 2 || y - lastY_[slot] < 0xc0) return;
  int idx = ram(0x0659 + slot);
  if (idx >= 0x30) return;
  int pos = ram(0x1b42 + 3 * idx);  // (level objects: id, page/column, row)
  int col = (pos >> 4) << 8 | (pos & 15) << 4, cam = cam16_ & 0x8000 ? cam16_ - 0x10000 : cam16_;
  if (col - cam > kSpawnReach43R) addRetry(idx, col);
}

void Smb3Engine::addRetry(int idx, int col) {
  if (idx >= 0x30 || retryCount_ == kMaxRetry) return;
  for (int i = 0; i < retryCount_; i++)
    if (retry_[i].idx == idx) return;
  retry_[retryCount_++] = Retry{(uint8_t)idx, (int16_t)col, 0};
}

// Sprite X written while an object draws itself: the low byte of its screen
// position plus an offset; recover the true position from the object's.
void Smb3Engine::onWrite(uint32_t adr, uint8_t v) {
  if (adr < 0x7e0800 || adr > 0x7e0a9f) return;
  // Sprites the game shares between objects (5-4's fire chains' seventh
  // ball) went to whichever drew last: at 4:3 the other was mostly out of
  // sight; in the wide view both are in it, and the first lost its ball. A
  // sprite an object in view has drawn this frame: another in view moves it
  // to a sprite nothing has drawn; one out of sight doesn't take it (it's put
  // back, and the rest of that sprite's writes undone).
  int i = adr >= 0x7e0a20 ? adr - 0x7e0a20 : (adr - 0x7e0800) >> 2;
  if (drawing_ >= 0 && oamKept_[i] == drawing_ + 1) {
    snes_.wram[adr & 0xffff] = adr >= 0x7e0a20 ? oamSnap_[i][4] : oamSnap_[i][adr & 3];
    return;
  }
  if (drawing_ >= 0 || (adr < 0x7e0a20 && (adr & 3) == 1 && v < 0xe0)) oamWritten_[i] = true;  // (not the frame's clearing)
  if (adr >= 0x7e0a20 || (adr & 3)) return;
  if (drawing_ < 0) {
    // Written again by something else (5-3: a Venus Fire Trap's new fireball
    // in a sprite a Piranha Plant had drawn earlier that frame): its own X,
    // not the plant's, or it flickered in beside the plant's pipe.
    oamXSet_[i] = false;
    return;
  }
  oamX_[i] = (int16_t)(drawingRel_ + (int8_t)(uint8_t)(v - (drawingRel_ & 0xff)));
  oamXSet_[i] = true;
  bool seen = ext_ > 0 && oamX_[i] + 8 > -extL_ && oamX_[i] < 256 + extR_;
  uint8_t* w = snes_.wram;
  if (!seen && oamOwner_[i] && oamOwner_[i] != drawing_ + 1) {
    for (int k = 0; k < 4; k++) w[0x0800 + 4 * i + k] = oamSnap_[i][k];
    w[0x0a20 + i] = oamSnap_[i][4];
    oamX_[i] = oamSnapX_[i];
    oamKept_[i] = (uint8_t)(drawing_ + 1);
  } else if (seen) {
    if (oamOwner_[i] && oamOwner_[i] != drawing_ + 1) {
      int j = 127;
      while (j >= 0 && (oamWritten_[j] || w[0x0801 + 4 * j] < 0xe0)) j--;
      if (j >= 0) {
        for (int k = 0; k < 4; k++) w[0x0800 + 4 * j + k] = oamSnap_[i][k];
        w[0x0a20 + j] = oamSnap_[i][4];
        std::memcpy(oamSnap_[j], oamSnap_[i], 5);
        oamX_[j] = oamSnapX_[j] = oamSnapX_[i];
        oamXSet_[j] = oamWritten_[j] = true;
        oamOwner_[j] = oamOwner_[i];
      }
    }
    oamOwner_[i] = (uint8_t)(drawing_ + 1);
  }
}

// After its initialisation (or when taken on from the level-load sweep), an
// object spawned beyond the 4:3 reach on auto-scroll: standing still, moving
// away, or already where the 4:3 game spawns it, it just runs; heading into
// view, it runs on the 4:3 timeline (kAhead) from where that has it now, or
// is parked until that is in view.
void Smb3Engine::leadIn(int slot) {
  uint8_t* w = snes_.wram;
  const Hold& h = hold_[slot];
  int step = (int8_t)h.vars[4] * 16, k = framesToSpawn(h.col);
  int cam = ram(0x0210) | ram(0x0211) << 8;
  int at = (h.vars[1] << 16 | h.vars[0] << 8 | h.vars[6]) - std::max(k, 0) * step;
  optional_[slot] = k > 0;
  if (step >= 0 || k == 0) {
    held_[slot] = kFree;
  } else if (k > 0 && (at >> 8) - cam <= 256 + extR_ + 16) {
    w[0x0044 + slot] = at >> 16;  // in view on the 4:3 timeline already
    w[0x005f + slot] = at >> 8;
    w[0x074f + slot] = at;
    held_[slot] = kAhead;
  } else if (!park(slot)) {
    held_[slot] = kWaiting;  // (no room to park it: it waits in its slot, not run)
  }
}


bool Smb3Engine::holdBefore() {
  CpuRegs& r = snes_.cpu.r;
  int slot = r.x & 0xff;
  if (slot > 7) return false;
  uint8_t* w = snes_.wram;
  int cam = ram(0x0210) | ram(0x0211) << 8;
  drawing_ = slot;
  drawingRel_ = (int16_t)(((w[0x0044 + slot] << 8) | w[0x005f + slot]) - cam);
  // (Not tied to the level NMI modes: objects the level-load sweep spawns run
  // for a frame or two before those are set.)
  uint8_t ts = ram(0x070a);
  bool on = active_ && want_ > 0 && ts >= 1 && ts <= 0x0e && ts != 7 && ram(0x0425) == 0 &&
            autoscroll();
  if (!holdInit_) {  // (after a reset: objects present now are not new)
    for (int i = 0; i < 8; i++) prevState_[i] = w[0x0661 + i];
    holdInit_ = true;
  }
  uint8_t st = w[0x0661 + slot];
  if (st == 0) {
    held_[slot] = kFree;
    optional_[slot] = optNormal_[slot] = false;
    if (slot <= 4) deleteRange_[slot] = -1;
  } else if (!on) {
    held_[slot] = kFree;
    if (prevState_[slot] == 0 && slot <= 4 && (spawnerMade_ & (1 << slot))) {
      // Normal scrolling: spawned where only the wide view reaches so far, it
      // gives way to what the 4:3 game has (see evictOptional).
      int col = ((w[0x0044 + slot] << 8) | w[0x005f + slot]) & ~15, rel = (int16_t)(col - cam);
      hold_[slot].col = (int16_t)col;
      optional_[slot] = optNormal_[slot] = active_ && want_ > 0 && ts >= 1 && ts <= 0x0e && ts != 7 &&
                                           ram(0x0425) == 0 && beyondReach43(rel);
    } else if (!optNormal_[slot]) {
      optional_[slot] = false;
    }
  } else if (prevState_[slot] == 0 && slot <= 4 && (spawnerMade_ & (1 << slot))) {
    // just spawned by the level's spawner (level objects use 0-4). (Not one
    // the game makes itself, as a donut lift's falling block where the player
    // stands on it: held back beyond the 4:3 reach, not run nor drawn, the
    // block vanished under the player in a margin, 6-7.)
    int col = ((w[0x0044 + slot] << 8) | w[0x005f + slot]) & ~15;
    held_[slot] = (int16_t)(col - cam) > kSpawnReach43R ? kWaiting : kFree;
    holdCaptured_[slot] = false;
    hold_[slot].col = (int16_t)col;
    returning_[slot] = false;
    optional_[slot] = false;
    for (int i = 0; i < parkedCount_; i++) {
      if (parked_[i].idx != w[0x0659 + slot]) continue;
      // A parked object back in view: it keeps its first start, and is put
      // where it is on the 4:3 timeline before it initialises (and draws).
      if (held_[slot] == kWaiting) {
        hold_[slot] = parked_[i].start;
        returning_[slot] = true;
        int k = framesToSpawn(hold_[slot].col);
        if (k > 0) {
          const Hold& h = hold_[slot];
          int x = (h.vars[1] << 16 | h.vars[0] << 8 | h.vars[6]) - k * (int8_t)h.vars[4] * 16;
          w[0x0044 + slot] = x >> 16;
          w[0x005f + slot] = x >> 8;
          w[0x074f + slot] = x;
          drawingRel_ = (int16_t)((x >> 8) - cam);
        }
      }
      parked_[i] = parked_[--parkedCount_];
      break;
    }
  }
  spawnerMade_ &= (uint8_t)~(1 << slot);
  if (held_[slot] != kWaiting || !holdCaptured_[slot]) return false;
  Hold& h = hold_[slot];
  auto restore = [&](int x) {
    for (int i = 0; i < kObjVarCount; i++) w[kObjVars[i] + slot] = h.vars[i];
    w[0x0044 + slot] = x >> 16;
    w[0x005f + slot] = x >> 8;
    w[0x074f + slot] = x;
    drawingRel_ = (int16_t)((x >> 8) - cam);
  };
  int x0 = h.vars[1] << 16 | h.vars[0] << 8 | h.vars[6];  // its start X, in 1/256 px
  int k = framesToSpawn(h.col);
  if (k == 0) {
    // The 4:3 game spawns it now (it moves from the next frame): its start.
    restore(x0);
    held_[slot] = kLast;
    return false;
  }
  if (k > 0) {
    int step = (int8_t)h.vars[4] * 16;  // X speed per frame, in 1/256 px (< 0)
    int at = x0 - k * step;             // where it is now, had it always been moving
    if ((at >> 8) - cam <= 256 + extR_ + 16) {
      restore(at - step);  // (its update moves it to `at`)
      held_[slot] = kAhead;
      return false;
    }
    restore(at);
  }
  // Still beyond the view (or the scroll stands): not run, not drawn.
  drawing_ = -1;
  r.pc = kObjUpdated & 0xffff;
  return true;
}

void Smb3Engine::holdAfter() {
  CpuRegs& r = snes_.cpu.r;
  int slot = r.x & 0xff;
  drawing_ = -1;
  for (int i = 0; i < 128; i++)  // (its sprites in view as it left them: see onWrite)
    if (oamOwner_[i] == slot + 1) {
      for (int k = 0; k < 4; k++) oamSnap_[i][k] = snes_.wram[0x0800 + 4 * i + k];
      oamSnap_[i][4] = snes_.wram[0x0a20 + i];
      oamSnapX_[i] = oamX_[i];
    }
  if (slot > 7) return;
  uint8_t* w = snes_.wram;
  Hold& h = hold_[slot];
  uint8_t st = w[0x0661 + slot];
  if (held_[slot] == kWaiting && !holdCaptured_[slot]) {
    // First update (its initialisation) done: how it starts out on the console
    // (an object spawned again keeps its first start).
    holdCaptured_[slot] = true;
    if (!returning_[slot]) captureHold(slot);
    returning_[slot] = false;
    leadIn(slot);
    st = w[0x0661 + slot];  // (0 if parked)
  } else if (held_[slot] != kFree && st != h.state) {
    held_[slot] = kFree;  // something happened to it (stomped, hit...): let it go
  } else if (held_[slot] == kLast) {
    for (int i = 0; i < kObjVarCount; i++) w[kObjVars[i] + slot] = h.vars[i];
    held_[slot] = kFree;
  } else if (held_[slot] == kAhead) {
    // Kept on the 4:3 timeline up to its 4:3 start, unless something changed
    // its course. The timeline assumes the scroll keeps its speed; where it
    // changes (it picks up at a level start), the object is eased back onto
    // it by at most 1/4 px a frame rather than jumping.
    bool course = w[0x008c + slot] == h.vars[4] && w[0x009e + slot] == h.vars[5] &&
                  w[0x0071 + slot] == h.vars[2] && w[0x0056 + slot] == h.vars[3];
    int k = framesToSpawn(h.col);
    if (course && k >= 0) {
      int target = (h.vars[1] << 16 | h.vars[0] << 8 | h.vars[6]) - k * (int8_t)h.vars[4] * 16;
      int x = w[0x0044 + slot] << 16 | w[0x005f + slot] << 8 | w[0x074f + slot];
      // (What is left spread over the frames left: on its 4:3 start then.)
      int d = target - x, lim = std::clamp(std::abs(d) / std::max(k, 1), 0x40, 0x100);
      if (k > 0) x += std::clamp(d, -lim, lim);
      else if (std::abs(d) <= 0x200) x = target;  // its 4:3 start, exactly
      w[0x0044 + slot] = x >> 16;
      w[0x005f + slot] = x >> 8;
      w[0x074f + slot] = x;
    }
    if (!course || k == 0) held_[slot] = kFree;
  }
  // Only the margins show it until the 4:3 game spawns it, unless something
  // happens to it (it may be carrying the player, too).
  if (optNormal_[slot]) {
    int rel = (int16_t)(h.col - (ram(0x0210) | ram(0x0211) << 8));
    if (!optional_[slot] || st == 0 || !beyondReach43(rel) || (prevState_[slot] == 2 && st != 2))
      optional_[slot] = optNormal_[slot] = false;
  } else if (optional_[slot] &&
      (st != h.state || (held_[slot] == kFree && ((h.vars[4] & 0x80) || framesToSpawn(h.col) == 0))))
    optional_[slot] = false;
  prevState_[slot] = st;
  lastY_[slot] = (int16_t)(w[0x0056 + slot] << 8 | w[0x0071 + slot]);
}

bool Smb3Engine::inWideView(int slot) const {
  int ox = ram(0x005f + slot) | ram(0x0044 + slot) << 8, cam = ram(0x0210) | ram(0x0211) << 8;
  int rel = (int16_t)(ox - cam);
  return rel > -extL_ - 16 && rel < 256 + extR_;
}

// In place of JSL $27A85F (A = $0651,X | $0681,X, the object's horizontal
// and vertical off-screen bits): horizontally, the view's range counts. On
// auto-scroll always (the player in the margins); elsewhere with the
// enemies active in full view (what's shown in the margins behaves as on the
// screen: a thrown shell, a tail, fire from an enemy there).
bool Smb3Engine::offscreenGate() {
  if (!gameplay_ || ext_ == 0 || vertical_ || !(autoscroll() || active_)) return false;
  CpuRegs& r = snes_.cpu.r;
  if (!r.mf || !r.xf) return false;
  int slot = r.x & 0xff;
  if (slot > 7) return false;
  uint8_t h = ram(0x0651 + slot);
  if (inWideView(slot)) h = 0;
  uint8_t v = h | ram(0x0681 + slot);
  r.a = (r.a & 0xff00) | v;
  r.n = v & 0x80;
  r.z = v == 0;
  r.pc += 4;
  return true;
}

// Columns that jump into the spawn range unchecked (see spawncatchup.h).
void Smb3Engine::trackSpawnGrowth() {
  if (swept_ || vertical_) {
    swept_ = false;  // the level-load sweep already covered the whole view
    spawn_.assumeCovered();
  }
  int cam = cam16_ & 0x8000 ? cam16_ - 0x10000 : cam16_;
  bool jumped = spawn_.track(extL_, extR_, [&](int m) { return (cam + 0x110 + m) & ~15; },
                             [&](int m) { return (cam - 0x20 - m) & ~15; });
  if (jumped) forgetParked();  // (they belong to the place the camera left)
}

// At the spawn routine ($29:9AE0, once per frame): point this frame's check
// at the next queued column; frameStart restores the offsets.
void Smb3Engine::catchUpSpawn() {
  if (sweeping_ || !active_ || ext_ == 0 || vertical_) return;
  int side = ram(0x0025) & 1;  // 0 = right, 1 = left (scroll direction)
  int cam = ram(0x0210) | (ram(0x0012) << 8);
  if (cam & 0x8000) cam -= 0x10000;
  int natural = side == 0 ? (cam + 0x110 + extR_) & ~15 : (cam - 0x20 - extL_) & ~15;
  int col;
  // Level controllers whose 4:3 spawn point has come first; then objects due
  // back, on every other frame (the rest keep the usual checks).
  bool due = dueControl(&col) ||
             ((parkedCount_ > 0 || retryCount_ > 0) && !(ram(0x0015) & 1) && dueColumn(&col));
  if (due) {
    if (col != natural) spawn_.displaced(side == 0, natural);
  } else if (!spawn_.pending() || !spawn_.next(side == 0, natural, &col)) {
    return;
  }
  int t = (col - cam) & 0xffff;
  patch(0x21c699 + side, t & 0xff);
  patch(0x21c69b + side, t >> 8);
}

// Spawn column offsets from the camera (lo/hi bytes are separate tables,
// indexed by side): right +$0110 + r, left -$0020 - l. `sweepHi` overrides
// the right offset's high byte for the level-load sweep (see onExec).
void Smb3Engine::patchSpawn(int l, int r, int sweepHi) {
  int right = 0x0110 + r, left = (0xffe0 - l) & 0xffff;
  patch(0x21c699, right & 0xff);
  patch(0x21c69b, sweepHi >= 0 ? sweepHi : (right >> 8) & 0xff);
  patch(0x21c69a, left & 0xff);
  patch(0x21c69c, left >> 8);
}

// Level load ($29:A1CE) spawns everything already on screen by moving a
// temporary camera across it and checking the left spawn column at each
// step, up to camera + (right - left offset). It runs before this frame's
// wide range is patched in (or with the previous area's range), so objects
// in the margins were skipped and never appeared (e.g. 1-1's first Venus Fire
// Trap). Widen that sweep to cover every view the new area can show.
namespace {
constexpr uint32_t kSweepStart = 0x29a1f3;  // after the vertical-level branch
constexpr uint32_t kSweepEnd = 0x29a240;    // loop done: restore the camera
constexpr uint32_t kSpawnCheck = 0x299ae0;  // horizontal spawn check (once per frame)
constexpr uint32_t kSpawnNoSlot = 0x299bc9;  // ... which found no free object slot
constexpr uint32_t kSpawned = 0x299c3f;      // ... which spawned level object $01
constexpr uint32_t kObjDeleted = 0x279c52;   // an object deleted off-screen (spawnable again)
constexpr uint32_t kDeleteRange = 0x279bee;  // the off-screen delete test, A = its range (0/2/4)
// Auto-scroll player bounds ($27:86BC-$86F4): at the pushing edge (screen X
// $79 <= $0F) or the far edge ($79 >= $E0) the player's X speed ($8B) is held
// to the scroll speed ($1A0E). $79 is 8 bits, so the player could never use
// the widescreen margins. The same test runs here on the 16-bit position
// relative to the camera, with the edges of the wide view as the bounds.
constexpr uint32_t kAutoscrollBounds = 0x2786bc;
// With the player free to use the margins on auto-scroll, two more tests of
// the 4:3 screen have to follow the wide view:
// * $23:C324: the player dies once pushed past the screen's right side
//   (8-bit screen X $79 >= $F8); now past the view's right side.
// * Interactions skip objects the game counts as off-screen (their 8-px
//   columns outside [0, 256), $0651), so enemies in the margins walked
//   through the player. Those bits also give the sprites' X bit 8, so they
//   stay as they are; instead the off-screen tests before the player
//   collision ($27:9916), tail hits ($27:A61C) and thrown objects hitting
//   others ($27:93BA) use the view's range during auto-scroll; elsewhere too
//   with the enemies active in full view (Original Timing: as the 4:3 game).
constexpr uint32_t kEdgeDeath = 0x23c324;
// A kicked shell hitting a block from the side ($27:940B, the wall bits in
// $A7,X) bumps it ($23:D83B: a coin, an item) unless the shell is at the
// game's screen edge ($27:9414: its screen X $7A,X >= $E4 moving right, < 6
// moving left) or counted off-screen ($27:9425, JSL $27:A85F: $0651): blocks
// it couldn't show. The margins show them (drawn from the level's memory):
// there, it bumps them too (1-1: a shell kicked right off the green block
// bounced off the coin block beyond the 4:3 screen, not bumping it).
constexpr uint32_t kShellEdge = 0x279414;
constexpr uint32_t kShellOffscreen = 0x279425;
// Object update loop ($27:9019): JSR $90EC runs slot X (movement, AI,
// drawing), then JSR $A5BB. With the wide enemy range, level objects spawn
// earlier, further out. On auto-scroll that changes the level: 1-4's
// drifting platforms, spawned a margin's width early, crossed the gaps long
// before the player got there. So there an object spawned beyond the 4:3
// reach that heads into view is kept on the 4:3 game's timeline: from its
// start (as its initialisation leaves it), the frame the 4:3 game would
// spawn it (the auto-scroll camera's speed is known) and its speed gives
// where it would be now had it always been moving. While that is beyond the
// view it is not run; once it comes in, it runs from there as normal, and
// reaches its 4:3 start in the frame the 4:3 game spawns it (kept to it until
// then, unless something changes its course). Objects standing still or
// moving away just run. Elsewhere (the camera follows the player) objects
// simply run from where they spawn: anything visible moves.
// (kObjUpdate / kObjUpdated are declared at the top.)
// NMI sprite upload (STZ $2102, then DMA from the $0800 buffer).
constexpr uint32_t kOamUpload = 0x20f9a7;
// (and the falling donut lift's ($54, $28:D3E2): off the 4:3 screen it
// deletes itself; the player stood on one in a margin, it vanished at once
// and he fell, 6-7)
// * ... and with the enemies active in full view, anywhere: an enemy in a
//   margin fires its projectiles ($27:8A7E, the free-slot search for them,
//   gives none to one off the 4:3 screen: 1-1's Venus Fire Trap just past it
//   never spat).
constexpr uint32_t kOffscreenGates[] = {0x279916, 0x27a61c, 0x2793ba, 0x28d3e2, 0x278a7e};  // JSL $27A85F
// Tests of $0651 written out in place, which with the enemies active in full
// view take the wide view's range:
// * $27:B892, the player's fireballs against each enemy (LDA $0651,Y; ORA
//   $0681,Y; BNE skip): they flew through enemies in the margins.
// * $27:A6F4 / $27:A714, a kicked shell against the others ($0651 & $C0 =
//   $C0: wholly off-screen, the shell's own, then each other's): the same.
// * $27:D1AD, a projectile held by its thrower (a Boomerang or Hammer Bro,
//   slot Y) deleted while he's off-screen: thrown in a margin, gone at once.
constexpr uint32_t kFireHitLoop = 0x27b892, kShellSelf = 0x27a6f4, kShellOther = 0x27a714, kThrowerGone = 0x27d1ad;
constexpr uint32_t kViewGates[] = {kFireHitLoop, kShellSelf, kShellOther, kThrowerGone};
// The goal card ($41), once taken, walks the player off to the right: its
// $28:CD4F hides his 12 sprites (from $C4) whose X wrapped past the screen's
// right edge (X < $40 -> Y = $F0; the hiding store is at $CD5E). It stops
// him ($0556 = $FF) at $28:CAE5 once his screen X $79 is $20, 32 px past it,
// and goes on to the tally at $28:CB2F on the same test.
constexpr uint32_t kGoalHide = 0x28cd4f, kGoalHideStore = 0x28cd5e, kGoalStop = 0x28cae5, kGoalOn = 0x28cb2f;
// Boss Bass swallows the player whole at $28:C6EA unless he has the star
// ($0553, $BF, $0587): the one way to be killed by an enemy that doesn't go
// through the hurt routine ($27:A4C6, which the grace after a hit stops).
constexpr uint32_t kBassSwallow = 0x28c6ea;
// The hurt routine's test for the grace, the star and the like ($27:A4DA, BEQ
// to the hit; else to the RTL at $A543): the Invincibility cheat takes the
// RTL. (Keeping the grace running ($0552) instead held up the airships' end:
// with the wand, the player didn't fall to the King's room until it ran out.)
constexpr uint32_t kHurtGate = 0x27a4da;
// A map panel's level looked up ($29:BF84 sets the layout $2B-$2D, the
// objects $31-$33 and the tileset): level select's airship replaces it here,
// from the world's airship pointers ($21:D7DD/$21:D7F5 + 3 * world, tileset
// $0A, as $29:C0E9 does), before the header is read; or a level with no panel
// of its own to find (kExtraLevels).
constexpr uint32_t kLevelLookedUp = 0x2086fb;
// Worlds 1-7's airships as the King's room leaves for them (its alternate
// area: the ship taking off from the castle, objects $27:FC4C, background 7,
// the stormy sky), on into the airship itself. The airship's own header has
// background 2, which loads none: entered straight from its pointers, it had
// a black sky (the storm is loaded only here, and kept on into it).
constexpr uint32_t kAirshipDeparture[7] = {0x26b8d6, 0x26b89c, 0x26b8b9, 0x26c0d1, 0x26c0ee, 0x26c10b, 0x26c128};
constexpr uint32_t kAirshipDepartureObj = 0x27fc4c;
// The free-slot search for objects the game makes itself ($27:DC85: slots
// 4-0, none free and it gives up): a donut lift's falling block where the
// player stands on it ($27:B0AE), and the like. The wide view's extra
// objects held all five slots at times, and the donut never fell (as the
// 4:3 game's would): one of them gives way, as for a spawn.
constexpr uint32_t kGameSlot = 0x27dc85;
// Bowser's castle's sprite palettes (Bowser's own, green: $3C:8A20 into
// sprite palette 4) are loaded at $29:CBD2 only in World 8, in the castle's
// tileset, and with the player's map position's screen ($7E3977 + player,
// saved as he entered) 3: the castle's place on the map. Level select's
// castle comes in through 1-1's panel (screen 2): Bowser was blue, as the
// palette every level starts with has him. The test reads 3 in the castle's
// areas however they were reached (a save state made in one too); the saved
// position stays (he goes back there if he loses a life).
constexpr uint32_t kCastlePage = 0x29cbd6;
// The level's frame: the player's update (JSL $23:BFA7 at $20:8F2F, back at $20:8F33).
constexpr uint32_t kPlayerCall = 0x208f2f, kPlayerRet = 0x208f33;
// Co-op's camera (see smb3coop.cpp): the horizontal scroll's code, its end,
// and the screen edge's hold on the player after it.
constexpr uint32_t kCamCode = 0x23d00a, kCamEnd = 0x23d093, kEdgeHold = 0x23d0c0;
// ... the vertical scroll's: the fixed bottom's test, its code, its end.
constexpr uint32_t kVCamLock = 0x23d177, kVCamCode = 0x23d18a, kVCamEnd = 0x23d1ef;
// ... and the fall's test (the player's screen Y $C0-$CF: in a pit) for one resting.
constexpr uint32_t kPitTest = 0x23c2d5;
// ... and the player's throw (fire, hammer: $27:B2BB-B2D9, JSR $B3A3), past its test.
constexpr uint32_t kThrowTest = 0x27b2dc;
// ... and the special objects' loop ($27:C536: fireballs, hammers, the wand...), each one's turn.
constexpr uint32_t kSpecUpdate = 0x27c53f, kSpecDone = 0x27c542;
// ... and the objects' run's start, where last frame's holding is passed on ($27:8FF4).
constexpr uint32_t kHoldCopy = 0x278ff4;
// ... and a coin taken ([a coin touched] $0564 = 1: $23:C038-C053 adds it,
// 5 points and the stats', each turn, until the objects' code clears it at
// $27:810A): in his turn, one taken in the game's player's is passed (one
// coin counted twice).
constexpr uint32_t kCoinCount = 0x23c038, kCoinCountEnd = 0x23c054;
// ... a platform standing its player on it ($28:B8C5: see coopPlatBefore),
// the tiles under the player's feet checked ($27:B29F, after JSR $B000: see
// coopTiles), and a Toad House chest's item put in the current player's
// inventory ($27:A8E1, LDY [Player_Current]: in co-op Luigi's turn put it in
// the two-player game's second inventory, never shown): Mario's.
constexpr uint32_t kPlatStand = 0x28b8c5, kTileCheck = 0x27b29f, kChestInv = 0x27a8e1;
// ... an auto-scroll's hold on the player ($27:8575, after JSR $8697: see
// coopAutoCarry), and the pipe move's exit stage (JSR $EBBA at $20:EA36,
// with [the pipe move] $0571 negative): not in Luigi's turn. The frame a
// pipe ends ($0571 = $8x, its area next) his turn ran it, with the stage a
// coin heaven leaves (7, its table has 0-5): a jump through code bytes, the
// game crashed. (He goes with the game's player, see coopExec.)
constexpr uint32_t kAutoCarry = 0x278575, kPipeStage = 0x20ea36;
// ... and the King's room's scene (the main loop's JSL $29:C280 there: no player update; see coopOam).
constexpr uint32_t kKingScene = 0x29c280;
// ... and the wand held up ($20:EADC-EB5F, from $20:EA2D: its sprite in
// entries 16-19, at the player's place): drawn in each player's update, the
// other's last: by whoever didn't catch it. The game's player's only.
constexpr uint32_t kWandDraw = 0x20eadc, kWandDrawEnd = 0x20eb5f;
constexpr uint32_t kContinueLivesA = 0x20b13b, kContinueLivesB = 0x20949b;
// ... and the player's two projectiles ($27:B42C: slot 1, then 0, $B436 each; back
// at $B433 after 1, at $B2B7 after 0): each with its thrower's variables.
constexpr uint32_t kProjUpdate = 0x27b436, kProjDone1 = 0x27b433, kProjDone0 = 0x27b2b7;
// The title's menu: its input ($20:B7EC), and its cursor's Y just written ($20:B82D).
constexpr uint32_t kTitleInput = 0x20b7ec, kTitleCursorY = 0x20b82d, kTitleStart = 0x20b8e1;  // (... and its start)
// The title's Koopas marching under the menu (four objects; X lo $55+k, hi
// $45+k, state $A6+k): each set off at X -16 ($20:C53E-C542, back at C544),
// and each frame the test that takes it away once off the 4:3 screen, X
// outside -16..271 (JSR $C3CE at $20:C56B, back at C56E). In the wide view they
// came and went at its 4:3 edges: set off past the margin's edge instead, and
// kept on until they're past the other.
constexpr uint32_t kTitleKoopaSet = 0x20c544, kTitleKoopaCull = 0x20c56b, kTitleKoopaCullRet = 0x20c56e;
// The lives a game starts with, from the save (LDA $7FFB02; STA $0736, both players'): just past it.
constexpr uint32_t kLivesSet = 0x2081be;
// A special object's touch on the player ($27:D302): X by the low bytes only
// ((ball + 6 - $5E) & $FF < 16, its high byte $E2,X not looked at), just past
// that test. On the 4:3 screen they're gone before they're 256 away; in the
// margins a cannonball by one brother hit the other, 256 to the right.
constexpr uint32_t kSpecHitX = 0x27d336, kSpecHitMiss = 0x27d384;
// The cannonball's own touch test ($27:CCA4) checks X by low byte too (+8, < $14) before its stomp
// check; on a side touch it jumps ($CD22) into the hurt part at kSpecHitX.
constexpr uint32_t kBallHitX = 0x27ccd7, kBallMiss = 0x27cd25, kBallHurt = 0x27cd22;
// A World 8 tank's deck hazards ($28:EC4B, X the object): their 8-bit screen X ($01) against the
// player's ($79), +4, < $10; past that, the hurt (JSL $27A4C6 at $EC97). In the wide view a player
// 256 px right of one (screen X 336 against 80) was hurt by it.
constexpr uint32_t kTankHitX = 0x28ec87, kTankMiss = 0x28ec9b;
// The objects' touch test ($27:A31F): 8-bit boxes from the screen X's ($79,
// the object's $7A,X) overlapped ($27:A45F), then (at $A36A) the 16-bit
// distance checked; a miss is its RTS at $A31A.
constexpr uint32_t kObjTouch = 0x27a36a, kObjTouchMiss = 0x27a31a;
}  // namespace

const Smb3Engine::ExtraLevel Smb3Engine::kExtraLevels[] = {
    {1, "Desert", 0x24c16c, 0x27edf3, 3},
    {1, "Pyramid", 0x25e05a, 0x27f6a7, 9},
    {4, "Fortress 2", 0x268ccb, 0x27f864, 2},
    {4, "Tower", 0x2698b4, 0x27fa9c, 2},
    {6, "Piranha Plant 1", 0x25c4cc, 0x27f62c, 5},
    {6, "Piranha Plant 2", 0x25c3bf, 0x27f618, 5},
    {7, "Tanks", 0x26c59f, 0x27ff07, 10},
    {7, "Battleship", 0x26c375, 0x27fe64, 10},
    {7, "Hand Trap 1", 0x25c644, 0x27f671, 11},
    {7, "Hand Trap 2", 0x25c58b, 0x27f643, 11},
    {7, "Hand Trap 3", 0x25c5f1, 0x27f660, 11},
    {7, "Super Tanks", 0x26c4a6, 0x27feb4, 10},
    {7, "Bowser's Castle", 0x26a051, 0x27fb7d, 2},
    // The Coin Ship (map object 11, $29:C163: per world at $21:D835/$21:D84D,
    // the same level in every world), under each world.
    {0, "Coin Ship", 0x26c7b3, 0x27ff9c, 10},
    {1, "Coin Ship", 0x26c7b3, 0x27ff9c, 10},
    {2, "Coin Ship", 0x26c7b3, 0x27ff9c, 10},
    {3, "Coin Ship", 0x26c7b3, 0x27ff9c, 10},
    {4, "Coin Ship", 0x26c7b3, 0x27ff9c, 10},
    {5, "Coin Ship", 0x26c7b3, 0x27ff9c, 10},
    {6, "Coin Ship", 0x26c7b3, 0x27ff9c, 10},
    {7, "Coin Ship", 0x26c7b3, 0x27ff9c, 10},
};
const int Smb3Engine::kExtraCount = sizeof(kExtraLevels) / sizeof(kExtraLevels[0]);

// The same for tests written out in place (see kViewGates): an object in the
// wide view isn't horizontally off-screen. (Its vertical bits stay.)
bool Smb3Engine::viewGate(uint32_t pc) {
  if (!gameplay_ || ext_ == 0 || vertical_ || !active_) return false;
  CpuRegs& r = snes_.cpu.r;
  if (!r.mf || !r.xf) return false;
  const bool byY = pc == kFireHitLoop || pc == kThrowerGone;
  const int slot = (byY ? r.y : r.x) & 0xff;
  if (slot > 7 || !inWideView(slot)) return false;
  uint8_t v = 0;
  int len = 3;  // (LDA $0651,X/Y)
  if (pc == kFireHitLoop) {
    v = ram(0x0681 + slot);  // (... ORA $0681,Y)
    len = 6;
  }
  r.a = (r.a & 0xff00) | v;
  r.n = v & 0x80;
  r.z = v == 0;
  r.pc += len;
  return true;
}

void Smb3Engine::markExec() {
  for (uint32_t b : {0x000000u, 0x800000u}) {
    snes_.markExec(kSweepStart | b, true);
    snes_.markExec(kSweepEnd | b, true);
    snes_.markExec(kSpawnCheck | b, true);
    snes_.markExec(kSpawnNoSlot | b, true);
    snes_.markExec(kSpawned | b, true);
    snes_.markExec(kSpawnMatch | b, true);
    snes_.markExec(kObjDeleted | b, true);
    snes_.markExec(kDeleteRange | b, true);
    snes_.markExec(kAutoscrollBounds | b, true);
    snes_.markExec(kEdgeDeath | b, true);
    snes_.markExec(kShellEdge | b, true);
    snes_.markExec(kShellOffscreen | b, true);
    snes_.markExec(kObjUpdate | b, true);
    snes_.markExec(kOamUpload | b, true);
    snes_.markExec(kObjUpdated | b, true);
    snes_.markExec(kObjDone | b, true);
    for (uint32_t g : kOffscreenGates) snes_.markExec(g | b, true);
    for (uint32_t g : kViewGates) snes_.markExec(g | b, true);
    snes_.markExec(kGoalHide | b, true);
    snes_.markExec(kGoalHideStore | b, true);
    snes_.markExec(kGoalStop | b, true);
    snes_.markExec(kGoalOn | b, true);
    snes_.markExec(kBassSwallow | b, true);
    snes_.markExec(kHurtGate | b, true);
    snes_.markExec(kLevelLookedUp | b, true);
    snes_.markExec(kGameSlot | b, true);
    snes_.markExec(kCastlePage | b, true);
    snes_.markExec(kPlayerCall | b, true);
    snes_.markExec(kPlayerRet | b, true);
    snes_.markExec(kCamCode | b, true);
    snes_.markExec(kCamEnd | b, true);
    snes_.markExec(kEdgeHold | b, true);
    snes_.markExec(kVCamLock | b, true);
    snes_.markExec(kVCamCode | b, true);
    snes_.markExec(kVCamEnd | b, true);
    snes_.markExec(kPitTest | b, true);
    snes_.markExec(kThrowTest | b, true);
    snes_.markExec(kSpecUpdate | b, true);
    snes_.markExec(kSpecDone | b, true);
    snes_.markExec(kHoldCopy | b, true);
    snes_.markExec(kCoinCount | b, true);
    snes_.markExec(kPlatStand | b, true);
    snes_.markExec(kTileCheck | b, true);
    snes_.markExec(kChestInv | b, true);
    snes_.markExec(kAutoCarry | b, true);
    snes_.markExec(kPipeStage | b, true);
    snes_.markExec(kKingScene | b, true);
    snes_.markExec(kWandDraw | b, true);
    snes_.markExec(kContinueLivesA | b, true);
    snes_.markExec(kContinueLivesB | b, true);
    snes_.markExec(kProjUpdate | b, true);
    snes_.markExec(kTitleInput | b, true);
    snes_.markExec(kTitleCursorY | b, true);
    snes_.markExec(kTitleStart | b, true);
    snes_.markExec(kTitleKoopaSet | b, true);
    snes_.markExec(kTitleKoopaCull | b, true);
    snes_.markExec(kLivesSet | b, true);
    snes_.markExec(kSpecHitX | b, true);
    snes_.markExec(kBallHitX | b, true);
    snes_.markExec(kBallHurt | b, true);
    snes_.markExec(kTankHitX | b, true);
    snes_.markExec(kObjTouch | b, true);
    snes_.markExec(kProjDone1 | b, true);
    snes_.markExec(kProjDone0 | b, true);
  }
}

bool Smb3Engine::onExec(uint32_t pc) {
  pc &= 0x7fffff;
  if (pc == kPlayerCall || pc == kPlayerRet) {
    inPlayer_ = pc == kPlayerCall;
    if (pc == kPlayerCall && coopTurn_ == 0) coopPadSwap();
    return pc == kPlayerRet && coopExec();
  }
  if (pc == kCamCode || pc == kCamEnd || pc == kEdgeHold || pc == kVCamLock || pc == kVCamCode || pc == kVCamEnd)
    return coopCamera(pc);
  if (pc == kThrowTest) return coopFire();
  if (pc == kObjTouch) {
    // The 8-bit boxes overlapped (and the 16-bit distance is under 128 each
    // way). Their screen X's: the player's wraps past the 4:3 screen's right
    // edge (co-op: Luigi held there), an object's ($7A,X) isn't kept up past
    // the view: an object just right of the wide view, 115 px from Luigi,
    // hurt him by a stale one. Then the boxes are compared by true X.
    if (extL_ == 0 && extR_ == 0 && !coop_) return false;
    const int x = snes_.cpu.r.x & 0xff, cam = ram(0x0210) | ram(0x0211) << 8;
    if (x > 7) return false;
    const int objRel = (ram(0x005f + x) | ram(0x0044 + x) << 8) - cam;
    const int plRel = (ram(0x005e) | ram(0x0043) << 8) - cam;
    // (only where the game's own distance test next lets it through: else it
    // misses there, as ever)
    const int dy = (int16_t)((ram(0x0070) | ram(0x0055) << 8) - (ram(0x0071 + x) | ram(0x0056 + x) << 8));
    if (std::abs(objRel - plRel) > 127 || std::abs(dy) > 127) return false;
    // (a screen X a few px from where it is now was set earlier in the frame,
    // as ever: the game's own test holds)
    auto near8 = [](int rel, int v) { return std::abs(((rel - v + 128) & 255) - 128) <= 8; };
    if (objRel >= 0 && objRel < 256 && plRel >= 0 && plRel < 256 && near8(objRel, ram(0x007a + x)) &&
        near8(plRel, ram(0x0079)))
      return false;
    const int pl = plRel + (int8_t)(ram(0x0002) - ram(0x0079)), ob = objRel + (int8_t)(ram(0x0000) - ram(0x007a + x));
    const int d = pl - ob;
    if (d >= 0 ? ram(0x0001) >= d : ram(0x0003) >= -d) return false;
    snes_.cpu.r.c = false;
    snes_.cpu.r.pc = (uint16_t)kObjTouchMiss;  // (its RTS: no touch)
    return true;
  }
  if (pc == kTankHitX) {
    if (extL_ == 0 && extR_ == 0 && !coop_) return false;
    // (the part's X: whichever of its 8-bit screen X's 256 px repeats lies nearest its tank)
    const int slot = snes_.cpu.r.x & 0xff, cam = ram(0x0210) | ram(0x0211) << 8;
    if (slot > 7) return false;
    const int obj = (ram(0x005f + slot) | ram(0x0044 + slot) << 8) - cam;
    int part = ram(0x0001);
    for (int c : {part - 256, part + 256})
      if (std::abs(c - obj) < std::abs(part - obj)) part = c;
    const int dx = part + 4 - ((ram(0x005e) | ram(0x0043) << 8) - cam);
    if (dx >= -1 && dx < 16) return false;
    snes_.cpu.r.pc = (uint16_t)kTankMiss;  // (its RTS: no touch)
    return true;
  }
  if (pc == kSpecHitX || pc == kBallHitX || pc == kBallHurt) {
    // A thing 256 px away from the player passes these low-byte tests; that only happens with
    // widescreen margins or a second brother, so the full X is checked then.
    if (extL_ == 0 && extR_ == 0 && !coop_) return false;
    if (pc == kBallHurt) {
      ballChecked_ = true;  // (its own 20 px test just passed in full: keep it)
      return false;
    }
    if (pc == kSpecHitX && ballChecked_) {
      ballChecked_ = false;
      return false;
    }
    const int x = snes_.cpu.r.x & 0xff, ball = pc == kBallHitX;
    const int dx = (ram(0x00e2 + x) << 8 | ram(0x05c9 + x)) + (ball ? 8 : 6) - (ram(0x0043) << 8 | ram(0x005e));
    if (dx >= 0 && dx < (ball ? 0x14 : 16)) return false;
    snes_.cpu.r.pc = (uint16_t)(ball ? kBallMiss : kSpecHitMiss);  // (their RTS: no touch)
    return true;
  }
  if (pc == kTitleInput || pc == kTitleCursorY || pc == kTitleStart || pc == kLivesSet || pc == kContinueLivesA ||
      pc == kContinueLivesB)
    return titleExec(pc);
  if (pc == kPlatStand) {
    standHit_ = true;
    return false;
  }
  if (pc == kTileCheck) return coopTiles();
  if (pc == kAutoCarry) return coopAutoCarry();
  if (pc == kKingScene) {
    kingFrame_ = true;
    return false;
  }
  if (pc == kWandDraw) {
    if (!(coop_ && inPlayer_ && coopTurn_ == 1)) return false;
    snes_.cpu.r.pc = kWandDrawEnd & 0xffff;  // (its RTS)
    return true;
  }
  if (pc == kPipeStage) {
    if (!(coop_ && inPlayer_ && coopTurn_ == 1)) return false;
    snes_.cpu.r.pc = 0xea39;  // (its RTL)
    return true;
  }
  if (pc == kChestInv) {
    if (!coop_ || ram(0x072b) >= 2) return false;
    snes_.cpu.r.y = 0;
    snes_.cpu.r.pc = 0xa8e8;
    return true;
  }
  if (pc == kTitleKoopaSet || pc == kTitleKoopaCull) {
    // (the margin as the title is shown: see SmasGame's picture screens; up
    // to 112, so -(m + 16) stays left of the PPU's split of the 9-bit X)
    const int m = atTitle() ? std::min(want_, 112) : 0;
    const int k = snes_.cpu.r.x & 3;
    if (m <= 0) return false;
    if (pc == kTitleKoopaSet) {
      const int x = -(m + 16);
      snes_.wram[0x45 + k] = (uint8_t)(x >> 8);
      snes_.wram[0x55 + k] = (uint8_t)x;
      return false;
    }
    const int x = (int16_t)(snes_.wram[0x45 + k] << 8 | snes_.wram[0x55 + k]);
    if (x < -(m + 16) || x >= 256 + m) return false;  // (gone: the game's test takes it away)
    snes_.cpu.r.pc = kTitleKoopaCullRet & 0xffff;
    return true;
  }
  if (pc == kProjUpdate) {
    coopProjBefore();
    return false;
  }
  if (pc == kProjDone1 || pc == kProjDone0) {
    coopObjAfter();
    // (After slot 0's run: More Active Fireballs' own, each returning here; see smb3fire.cpp.)
    return pc == kProjDone0 && fireExec();
  }
  if (pc == kCoinCount) {
    if (!(coop_ && inPlayer_ && coopTurn_ == 1 && coinHeld_)) return false;
    snes_.cpu.r.pc = kCoinCountEnd & 0xffff;
    return true;
  }
  if (pc == kHoldCopy) {
    coopHoldCopy();
    return false;
  }
  if (pc == kSpecUpdate) {
    coopSpecBefore();
    return false;
  }
  if (pc == kSpecDone) {
    coopObjAfter();
    return false;
  }
  if (pc == kPitTest) {
    if (coop_ && inPlayer_ && coopRests(coopTurn_)) {  // (resting where the other stood: below the view, not fallen)
      snes_.cpu.r.pc = 0xc2f0;
      return true;
    }
    return false;
  }
  if (pc == kLevelLookedUp) {
    if (swapPending_) {  // (the panel level select pressed A on)
      int w = std::min<int>(ram(0x0727), 7);
      uint32_t lay, obj;
      uint8_t ts = 0x0a;
      if (swapPending_ == kWarpAirship && w < 7) {
        lay = kAirshipDeparture[w];
        obj = kAirshipDepartureObj;
      } else if (swapPending_ == kWarpAirship) {
        lay = snes_.peek(0x21d7dd + 3 * w) | snes_.peek(0x21d7de + 3 * w) << 8 | snes_.peek(0x21d7df + 3 * w) << 16;
        obj = snes_.peek(0x21d7f5 + 3 * w) | snes_.peek(0x21d7f6 + 3 * w) << 8 | snes_.peek(0x21d7f7 + 3 * w) << 16;
      } else {
        const ExtraLevel& e = kExtraLevels[swapPending_ - kWarpExtra];
        lay = e.lay;
        obj = e.obj;
        ts = e.tileset;
      }
      for (int i = 0; i < 3; i++) {
        snes_.wram[0x2b + i] = (uint8_t)(lay >> (8 * i));
        snes_.wram[0x31 + i] = (uint8_t)(obj >> (8 * i));
      }
      snes_.wram[0x070a] = ts;
      swapPending_ = 0;
    }
    return false;
  }
  if (pc == kSweepStart) {
    areaStamp_++;  // (co-op: Luigi joins Mario in it)
    // A new area: nothing parked or latched carries over into it (1-4's goal
    // area kept the auto-scroll view's offset).
    wallsStale_ = true;
    forgetParked();
    forgetHeldControls();
    autoL_ = -1;
    easing_ = roomGlide_ = false;
    uint8_t ts = ram(0x070a);
    int e = std::min(want_, 112);
    if (!active_ || e <= 0 || ts < 1 || ts > 0x0e || ts == 7) return false;
    // (Single-page areas are shown 4:3: nothing beside their screen is shown,
    // and an object listed past their end appeared in one in World 6.)
    if (ram(0x0425) == 0 && ram(0x0022) == 0) return false;
    int cam = ram(0x0210) | (ram(0x0012) << 8);
    if (cam & 0x8000) cam -= 0x10000;
    int l = std::clamp(cam, 0, e), r = e;  // (the view is centred on the game's screen)
    // The sweep ends at camera + 256 * hi(right) + lo(right - left): give it
    // the high byte of the full width (it may carry past the right offset's).
    patchSpawn(l, r, ((0x0130 + l + r) >> 8) & 0xff);
    sweeping_ = true;
    sweepCam_ = cam;
  } else if (pc == kSpawnCheck) {
    catchUpSpawn();
  } else if (pc == kSpawnNoSlot) {
    spawnFailed();
  } else if (pc == kSpawned) {
    if ((snes_.cpu.r.x & 0xff) <= 4) spawnerMade_ |= (uint8_t)(1 << (snes_.cpu.r.x & 0xff));
    spawnedOne();
  } else if (pc == kSpawnMatch) {
    return holdControl();
  } else if (pc == kObjDeleted) {
    objectDeleted();
  } else if (pc == kDeleteRange) {
    int slot = snes_.cpu.r.x & 0xff;
    if (slot <= 4) deleteRange_[slot] = (int8_t)(snes_.cpu.r.a & 0xff);
  } else if (pc == kAutoscrollBounds) {
    return autoscrollBounds();
  } else if (pc == kEdgeDeath) {
    if (coop_ && (coopTurn_ == 1 || coopRests(0)) && !autoscroll()) {  // (co-op: held in the view, not crushed by it)
      snes_.cpu.r.pc = 0xc33d;
      return true;
    }
    if (coop_ && inPlayer_ && autoscroll() && coopRests(coopTurn_) && snes_.cpu.r.mf && coopCanBring(coopTurn_)) {
      // (Auto-scrolling, resting: the edge brings him along rather than crush
      // him; to the other, in play and in view. Else crushed, as the game's:
      // brought to the other dead behind the same wall, he never died, the
      // other came back beside him off the screen and died again, a life each
      // time, till none, nobody in view.)
      const int x = ram(0x005e) | ram(0x0043) << 8, cam = ram(0x0210) | ram(0x0211) << 8;
      if (x - cam < 8 - extL_ || x - cam > 248 + extR_) {
        coopBring(coopTurn_);
        snes_.cpu.r.pc = 0xc33d;
        return true;
      }
    }
    return edgeDeath();
  } else if (pc == kShellEdge || pc == kShellOffscreen) {
    return shellBump(pc == kShellEdge);
  } else if (pc == kOamUpload) {
    // The buffer written during the last game frame goes to the PPU now.
    Ppu& ppu = snes_.ppu;
    // Paused ($037A), only the pause box is drawn: centred on the display
    // rather than on the game's screen (they differ while the view is held).
    int shift = (extR_ - extL_) / 2;
    bool paused = gameplay_ && ext_ > 0 && ram(0x037a) != 0 && shift != 0;
    // Sprites not drawn by an object's update (see onWrite: while Mario goes
    // into a pipe the objects are drawn but not run) have only their 9-bit X,
    // and objects live further out than it tells apart: 5-4's fire chain 176
    // px past the left edge showed in the right margin. Such a sprite near a
    // margin's outer edge, with no object near it but one 512 px off, is put
    // there.
    int vx[9], live = 0;
    if (gameplay_ && ext_ > 0 && !vertical_) {
      int cam = ram(0x0210) | ram(0x0211) << 8;
      for (int k = 0; k < 8; k++)
        if (ram(0x0661 + k)) vx[live++] = (int16_t)((ram(0x005f + k) | ram(0x0044 + k) << 8) - cam);
      if (live) vx[live++] = (int16_t)((ram(0x005e) | ram(0x0043) << 8) - cam);  // (and the player's own)
    }
    auto near = [&](int x) {
      for (int k = 0; k < live; k++)
        if (x >= vx[k] - 48 && x <= vx[k] + 56) return true;
      return false;
    };
    // The player's masks (tile $1FE, behind the scenery: what they cover of
    // him, entering a pipe, shows the scenery) are put at his 8-bit screen X:
    // with him in a margin, 256 px off, and he showed in front of the pipe
    // (6-7's last pipe). They go where he is.
    int playerRel = -10000;
    if (gameplay_ && ext_ > 0 && !vertical_)
      playerRel = (int16_t)((ram(0x005e) | ram(0x0043) << 8) - (ram(0x0210) | ram(0x0211) << 8));
    int split = 256 + extR_ + std::max(0, 256 - extL_ - extR_) / 2;  // (as Ppu::renderObj reads X)
    for (int i = 0; i < 128; i++) {
      ppu.objTrueXSet[i] = oamXSet_[i] && ext_ > 0;
      ppu.objTrueX[i] = oamX_[i];
      oamXSet_[i] = false;
      oamOwner_[i] = oamKept_[i] = 0;
      oamWritten_[i] = false;
      int x9 = ram(0x0800 + 4 * i) | ((ram(0x0a20 + i) & 1) << 8);
      if (paused) {
        ppu.objTrueX[i] = (int16_t)((x9 >= 256 ? x9 - 512 : x9) + shift);
        ppu.objTrueXSet[i] = true;
      } else if (playerRel > -10000 && !ppu.objTrueXSet[i] && (ram(0x0802 + 4 * i) | (ram(0x0803 + 4 * i) & 1) << 8) == 0x1fe &&
                 (playerRel < 0 || playerRel >= 256)) {
        int sx = x9 < split ? x9 : x9 - 512;
        for (int d : {-256, 256})
          if (std::abs(sx + d - playerRel - 4) <= 24) {
            ppu.objTrueX[i] = (int16_t)(sx + d);
            ppu.objTrueXSet[i] = true;
          }
      } else if (live && !ppu.objTrueXSet[i]) {
        int sx = x9 < split ? x9 : x9 - 512, alt = sx < 0 ? sx + 512 : sx - 512;
        bool outer = sx < 0 ? sx < 64 - extL_ : sx >= 256 + extR_ - 64;
        if (outer && !near(sx) && near(alt)) {
          ppu.objTrueX[i] = (int16_t)alt;
          ppu.objTrueXSet[i] = true;
        }
      }
    }
    coopOam(ppu);
  } else if (pc == kObjUpdate) {
    coopObjBefore();
    if (holdBefore()) return true;
    coopPlatBefore();
    return false;
  } else if (pc == kObjUpdated) {
    if (coopPlatAfter()) return true;
    holdAfter();
  } else if (pc == kObjDone) {
    coopObjAfter();
  } else if (std::find(std::begin(kOffscreenGates), std::end(kOffscreenGates), pc) != std::end(kOffscreenGates)) {
    return offscreenGate();
  } else if (std::find(std::begin(kViewGates), std::end(kViewGates), pc) != std::end(kViewGates)) {
    return viewGate(pc);
  } else if (pc == kGoalHide) {
    goalWalk();
  } else if (pc == kGoalStop) {
    return goalStop(false);
  } else if (pc == kGoalOn) {
    return goalStop(true);
  } else if (pc == kCastlePage) {
    int obj = ram(0x0031) | ram(0x0032) << 8 | ram(0x0033) << 16;
    if (ram(0x0727) == 7 && (obj == 0x27fb7d || obj == 0x27fbbb))  // (the castle's two areas' objects)
      snes_.cpu.r.a = (snes_.cpu.r.a & 0xff00) | 3;
  } else if (pc == kGameSlot) {
    bool full = true;
    for (int s = 0; s <= 4 && full; s++) full = ram(0x0661 + s) != 0;
    if (full && gameplay_ && ext_ > 0) evictOptional();
  } else if (pc == kHurtGate && (noDamage_ || coopIdleSafe())) {
    snes_.cpu.r.pc = 0xa543;  // (not hurt)
    return true;
  } else if (pc == kBassSwallow && (noDamage_ || coopIdleSafe())) {
    snes_.cpu.r.pc = 0xc702;  // (not swallowed: as with the star)
    return true;
  } else if (pc == kGoalHideStore) {
    // (one the wide view shows: not hidden)
    int i = (snes_.cpu.r.y & 0x1ff) >> 2;
    if (goalShown_[i]) {
      snes_.cpu.r.pc = (kGoalHideStore + 5) & 0xffff;
      return true;
    }
  } else if (pc == kSweepEnd && sweeping_) {
    sweeping_ = false;
    swept_ = true;
    adoptPending_ = true;
    if (active_ && ext_ > 0) {
      patchSpawn(extL_, extR_);
    } else {
      for (uint32_t a : {0x21c699u, 0x21c69au, 0x21c69bu, 0x21c69cu})
        if (original_.count(a)) snes_.poke(a, original_[a]);
    }
  }
  return false;
}

// Where the wide view sits around the game's 256 px screen: `extL` px of it
// to the left, the rest (2 * ext - extL) to the right. The game's camera is
// never changed; only the view moves.
// * The view is centred on the game's screen, as at 4:3, the level ends and
//   boss rooms too: the level carried on past its ends (see planFill), the
//   player kept inside it (see autoscrollBounds).
// * Auto-scroll: the view keeps the offset it had when the scrolling began
//   (centred, unless the width changed or a state was loaded mid-scroll) and
//   moves with the game's camera, so the picture scrolls exactly as steadily
//   as the game's. The player's range is widened to the view, within the
//   level (kAutoscrollBounds).
// (the game's auto-scroll on ($0580) with a speed: the speed is left over in an
// area entered from an auto-scrolling one, 7-2's pipe area, its view held as
// if scrolling and its room only found when the hold lapsed)
bool Smb3Engine::autoscroll() const { return ram(0x0580) != 0 && (ram(0x1a0e) != 0 || ram(0x1a14) != 0); }

void Smb3Engine::currentSplit(int* l, int* r) const {
  int cam = cam16_ & 0x8000 ? cam16_ - 0x10000 : cam16_;
  *l = autoL_ >= 0 ? std::clamp(autoL_, 0, 2 * ext_)
       : boxed_ ? std::clamp(ext_ + cam + 128 - boxCentre_, 0, 2 * ext_)
                       : ext_;
  *r = 2 * ext_ - *l;
}

// Walls the player can't get past, either side of him: 4 columns or more of
// tiles solid all the way down the rows in view (as the game tells them:
// see hardTile). What is beyond one is another part of the level, which the
// game's screen never shows from here (the World 2 fortress's sections,
// 2-1's pipe rooms): the wall is carried on over it, by the run of 1-4 of its
// columns that repeats (big blocks: 2), and what moves there isn't drawn.
// Between two walls 8-18 columns apart (or a wall and the level's end) under a
// ceiling is a room: the view stays centred on it (2-1's and 4-5's pipe rooms,
// the fortress boss rooms), however the game's camera wanders in it. A room's
// own frame at the level's end (2-1's first pipe room) has the far wall
// carried on past it too.
void Smb3Engine::findWallsIn() {
  beyond_[0] = beyond_[1] = Beyond{};
  boxed_ = false;
  if (!gameplay_ || vertical_ || onePage_ || ext_ == 0 || autoL_ >= 0) return;
  int camY = ram(0x0216) | ram(0x0217) << 8, last = (ram(0x0022) + 1) * 16 - 1;
  // (the rows in view, half of each at least; the room's ceiling may be one
  // barely in view: the World 2 fortress's walls end there, 2-1's rooms'
  // frames start there)
  int rc = std::clamp(camY >> 4, 0, kFillRows - 1);
  int r0 = std::clamp((camY + 8) >> 4, 0, kFillRows - 1), r1 = std::clamp((camY + 183) >> 4, 0, kFillRows - 1);
  auto meta = [&](int c, int row) { return tileMeta((c >> 4) * 0x1b0 + row * 16 + (c & 15)); };
  int px = (ram(0x0043) << 8 | ram(0x005e)) + 8, py = (int16_t)(ram(0x0055) << 8 | ram(0x0070)) + 8;
  int pc = px >> 4, pr = py >> 4;
  // (Flying just above the screen's top (7-7's start wall) is still in the
  // room; his own row isn't used below. Not far above it, in the solid over
  // a room: the World 1 fortress's moving-floor room, the QA sweep's
  // player held up there boxed it.)
  if (pc < 0 || pc > last || pr > r1 || pr < r0 - 4) return;
  auto solid = [&](int c) {
    if (c < 0 || c > last) return false;
    for (int row = r0; row <= r1; row++)
      if (!hardTile(meta(c, row))) return false;
    return true;
  };
  if (solid(pc)) return;
  // (A wall goes on down to the level's bottom, or the lava or water along
  // it: a block the view is filled with only while it's high up, open space
  // under it, is not one: 6-5's underground, flying up between its big
  // blocks, the cave beside them was walled over.)
  // (Or down as far as the floor the player is over goes: what is under both
  // is another way, not his: the World 2 fortress's upper rooms, a passage
  // under their floor.)
  auto deep = [&](int c) {
    if (c < 0 || c > last) return true;
    int t = meta(c, kFillRows - 1), bot = kFillRows;
    if (!hardTile(t))
      while (bot > r1 + 1 && meta(c, bot - 1) == t) bot--;
    int open = -1;
    for (int row = r1 + 1; row < bot && open < 0; row++)
      if (!hardTile(meta(c, row))) open = row;
    if (open < 0) return true;
    int f = std::max(pr, 0);
    while (f < open && !hardTile(meta(pc, f))) f++;
    if (f >= open) return false;
    for (int row = f; row < open; row++)
      if (!hardTile(meta(pc, row))) return false;
    // (the floor all the way across to it, at that depth: not a pillar he
    // stands on, the way down beside it going on under the wall, 4-4's
    // tower of blocks over its passage)
    for (int k = std::min(pc, c) + 1; k < std::max(pc, c); k++)
      if (!hardTile(meta(k, open - 1))) return false;
    return true;
  };
  // (Nor a block beside a shaft, a wall the other side too, the shaft going
  // on up into a passage over the block: the World 8 castle's, walled as
  // the passage left the view, half way down, and from the room the shaft
  // comes down into, the castle's rooms beyond it gone as the player walked
  // on. Not a room's wall the room goes on over, out of view: the World 2
  // fortress's big blocks.)
  auto openAbove = [&](int c, int side) {
    int o = side ? -1 : 1, k = c + o;  // (the column inside its face)
    bool shaft = false;
    // (the shaft as it goes on up, just above the view: lower down it opens
    // into the next room)
    if (r0 < 1 || k < 0 || k > last) return false;
    for (int j = 1; j <= 7 && !shaft; j++) shaft = k + o * j >= 0 && k + o * j <= last && hardTile(meta(k + o * j, r0 - 1));
    if (!shaft) return false;
    for (int row = r0 - 1; row >= 0; row--) {
      if (hardTile(meta(k, row))) return false;
      if (!hardTile(meta(c, row))) return true;
    }
    return false;
  };
  int nearCol[2] = {-1, last + 1};  // (the level's ends, if no wall)
  int anyCol[2] = {-1, last + 1}, anyRun[2] = {0, 0};  // the nearest solid columns, any width
  for (int side = 0; side < 2; side++) {
    int d = side ? 1 : -1;
    for (int c = pc + d, n = 0; c >= 0 && c <= last && n < 40; c += d, n++) {
      if (!solid(c)) continue;
      int e = c;
      while (solid(e + d)) e += d;
      int a = std::min(c, e), b = std::max(c, e);
      if (anyRun[side] == 0) {
        anyCol[side] = c;
        anyRun[side] = b - a + 1;
      }
      if (b - a + 1 < 4 || !deep(c)) continue;  // (a pipe, a post)
      if (openAbove(c, side)) break;
      nearCol[side] = c;
      if (side ? b >= last : a <= 0) break;  // (to the level's end: past it is as past any end)
      // the run of its columns that repeats inside it (its faces, a room's
      // frame, at its edges)
      int p = 0;
      for (int q = 1; q <= 4 && !p && 2 * q <= b - a - 1; q++) {
        bool rep = true;
        for (int k = a + 1; k + q <= b - 1 && rep; k++)
          for (int row = r0; row <= r1 && rep; row++) rep = meta(k, row) == meta(k + q, row);
        if (rep) p = q;
      }
      Beyond& z = beyond_[side];
      z.on = true;
      // (from its far face, if it has one: the next room's frame; none
      // inside: the whole of it, one big block with its outline, repeated)
      z.from = p ? (side ? b * 16 : (a + 1) * 16) : side ? (b + 1) * 16 : a * 16;
      z.start = p ? a + 1 : a;
      z.edge = side ? (b + 1) * 16 : a * 16;
      z.period = p ? p : b - a + 1;
      z.show = side ? c * 16 : (c + 1) * 16;
      break;
    }
  }
  // A room between them? (A solid column at the level's end is its frame.)
  // Its walls may be thin, one or two columns between rooms (3-9's pipe
  // rooms): then those are its walls, and beyond them the walls' material
  // is carried on (repeating a frame column made a row of frames): the
  // checkered blocks, the same at any height.
  // (A thin-walled room's ceiling may be above the view, the player down in
  // the room: 3-9's pipe rooms are two screens tall. Its walls reach up to
  // it. Others' is in view.)
  auto ceilingOver = [&](int l, int r, bool above) {
    for (int row = r1 - 1; row >= (above ? 0 : rc); row--) {
      if (row < rc && ((l >= 0 && !hardTile(meta(l, row))) || (r <= last && !hardTile(meta(r, row))))) return false;
      bool full = true, under = false;
      for (int c = l + 1; c < r && full; c++) full = hardTile(meta(c, row));
      for (int c = l + 1; c < r && full && !under; c++)
        for (int y = std::max(row + 1, rc); y <= r1 && !under; y++) under = !hardTile(meta(c, y));
      if (full && under) return true;
    }
    return false;
  };
  auto isRoom = [&](int l, int r, bool above) {
    int inside = r - l - 1;
    return inside >= 8 && inside <= 18 && !(l < 0 && r > last) && ceilingOver(l, r, above);
  };
  for (int side = 0; side < 2; side++)
    if (side ? anyCol[1] > last : anyCol[0] < 0) {
      if (solid(side ? last : 0)) anyCol[side] = side ? last : 0;
      if (side ? nearCol[1] > last : nearCol[0] < 0) nearCol[side] = anyCol[side];
    }
  int l = nearCol[0], r = nearCol[1];
  // A screen held still ($1EB8: Boom-Boom's room, the camera no longer
  // follows the player, $23:CFD2, and he's kept on it) with a thin wall at
  // its edge, down to the floor: past it was never in view. The W7
  // fortress's, one column thick, the hall carried on past it. Its blocks
  // carry on there, as a thin wall's (below); the view isn't moved.
  bool held = ram(0x1eb8) != 0;
  int heldCol[2] = {-1, -1};
  if (held) {
    int camX = ram(0x0210) | ram(0x0211) << 8;
    // (The screen's own edge column: the nearest solid one to the player was
    // at times a slope's band in view, 8-1's underground room (also held),
    // its wall gone for a frame.)
    for (int side = 0; side < 2; side++) {
      int c = side ? (camX + 255) >> 4 : camX >> 4;
      // (not a thick one, as above: Bowser's room's, to the level's end)
      if (c < 0 || c > last || c == nearCol[side] || beyond_[side].on || !solid(c) || !deep(c)) continue;
      heldCol[side] = c;
    }
  }
  bool heldWall = heldCol[0] >= 0 || heldCol[1] >= 0;
  if (heldWall) {  // (the material from about the screen)
    l = heldCol[0] >= 0 ? heldCol[0] : heldCol[1] - 17;
    r = heldCol[1] >= 0 ? heldCol[1] : heldCol[0] + 17;
  }
  bool thin = !heldWall && (anyCol[0] != l || anyCol[1] != r) && (deep(anyCol[0]) || deep(anyCol[1])) &&
              isRoom(anyCol[0], anyCol[1], true);
  // A shaft between thin walls, 1-7 columns wide, solid all the way up the
  // view (5-5's way down into its pipe room): its walls carried on too, the
  // view left where it is (it's centred on the room below once there).
  // Beyond its wall was the open level left of the room.
  bool shaft = !thin && !heldWall && anyCol[0] >= 0 && anyCol[1] <= last && anyCol[1] - anyCol[0] - 1 >= 1 &&
               anyCol[1] - anyCol[0] - 1 <= 7 && (anyRun[0] < 4 || anyRun[1] < 4) && (deep(anyCol[0]) || deep(anyCol[1])) &&
               !isRoom(l, r, false);
  if (thin || shaft || heldWall) {
    if (!heldWall) {
      l = anyCol[0];
      r = anyCol[1];
    }
    // (The material is the blocks' inside, a solid tile with the same tile on
    // all four sides, the commonest about the room at any height: what's in
    // view changes as the player climbs, and the commonest tile there was a
    // wall's face at times, carried on as stripes. Else the commonest solid
    // tile beside it. It's carried on straight from the wall's own column:
    // the blocks past that, in view at some heights, showed their outlines.)
    std::map<int, int> count, inner;
    for (int c = std::max(0, l - 10); c <= std::min(last, r + 10); c++)
      for (int row = 0; row < kFillRows; row++) {
        int t = meta(c, row);
        if (!hardTile(t)) continue;
        if (c <= l || c >= r) count[t]++;
        if (c > 0 && c < last && row > 0 && row < kFillRows - 1 && meta(c - 1, row) == t && meta(c + 1, row) == t &&
            meta(c, row - 1) == t && meta(c, row + 1) == t)
          inner[t]++;
      }
    // (First the room's own: its ceiling's and floor's blocks between its
    // walls. Round it, the World 4 fortress's checkered room had the next
    // room's frame of blue blocks, a bigger mass, carried on both sides.)
    std::map<int, int> own;
    for (int c = std::max(1, l + 1); c <= std::min(last - 1, r - 1); c++)
      for (int row = 1; row < kFillRows - 1; row++) {
        int t = meta(c, row);
        if (hardTile(t) && !isPipe(t) && meta(c - 1, row) == t && meta(c + 1, row) == t && meta(c, row - 1) == t &&
            meta(c, row + 1) == t)
          own[t]++;
      }
    int material = -1, most = 0;
    for (auto& kv : !own.empty() ? own : inner.empty() ? count : inner)
      if (kv.second > most) material = kv.first, most = kv.second;
    if ((shaft || heldWall) && inner.empty()) return;  // (no blocks' inside to carry on)
    // (A thick wall keeps its own carry-on: 4 columns solid at every height,
    // not just in view, or the side changed as the player climbed.)
    auto fullCol = [&](int c) {
      if (c < 0 || c > last) return false;
      for (int row = 0; row < kFillRows; row++)
        if (!hardTile(meta(c, row))) return false;
      return true;
    };
    for (int side = 0; side < 2; side++) {
      int c = side ? r : l, d = side ? 1 : -1, n = 0;
      if (heldWall && heldCol[side] < 0) continue;
      if (c < 0 || c > last || (side ? c >= last : c <= 0)) continue;
      // (A wall the room goes on under, open both sides of it there, isn't
      // one: 8-2's pipe room, its left wall stops above the floor, the shaft
      // up to its pipe beyond it.)
      if (thin && !deep(c)) {
        int row = r1 + 1;
        while (row < kFillRows && hardTile(meta(c, row))) row++;
        if (row < kFillRows && !hardTile(meta(c - d, row))) continue;
      }
      while (n < 4 && fullCol(c + n * d)) n++;
      if (n >= 4) continue;
      Beyond& z = beyond_[side];
      z = Beyond{};
      z.on = true;
      z.from = side ? (c + 1) * 16 : c * 16;
      z.tile = material;
      // (its far face, as far as it's solid in view: what the 4:3 camera has
      // to reach to show past it (see findWalls); 4-5's bonus room's right
      // wall, 7 columns, taken as one, came out the next room)
      int run = c == anyCol[side] ? std::max(anyRun[side], 1) : 1;
      z.edge = side ? (c + run) * 16 : (c - run + 1) * 16;
      z.show = side ? c * 16 : (c + 1) * 16;
      // (A floor through the wall, repeating every 1-2 tiles in it and inside:
      // the World 5 fortress's checkered floor under its wall. It carries on.)
      for (int row = 0; row < kFillRows; row++)
        for (int p = 1; p <= 2 && !z.bandP[row]; p++) {
          bool rep = meta(c, row) != material || meta(c - d, row) != material;
          for (int k = 0; k < 4 && rep; k++)
            rep = hardTile(meta(c - k * d, row)) && meta(c - k * d, row) == meta(c - (k + p) * d, row);
          if (rep) {
            z.bandP[row] = (int8_t)p;
            z.bandS[row] = (int16_t)(side ? c - p + 1 : c);
          }
        }
    }
    if (shaft || heldWall) return;
  } else if (!isRoom(l, r, false)) {
    return;
  }
  boxed_ = true;
  boxCentre_ = (l + 1 + r) * 8;
  for (int side = 0; side < 2; side++) {
    Beyond& z = beyond_[side];
    const Beyond& o = beyond_[side ^ 1];
    if (!z.on && (side ? r == last : l == 0) && o.on) {
      z = o;
      z.on = true;
      z.from = side ? (last + 1) * 16 : 0;
      z.show = z.from;
      z.edge = z.from;
    }
  }
}

// What the walls hide is only what the game's own screen doesn't show: a
// wall it can show past, the player in the room (its camera keeps him 112 px
// from its left edge, 128 from its right, at most; the level's ends hold it),
// is no wall, nor its room (7-2's pipe area: a pillar of blocks 4 wide, the
// next room beyond in view at 4:3). The margins show the level there as the
// 4:3 picture does. A thin wall it can show past is no wall either, on its
// own side only (3-9's and 4-5's rooms' it can't).
void Smb3Engine::findWalls() {
  // (In a pipe ($9C: the player's special state), the view going up or down
  // through it, slices of the level came into view as walls and rooms, and
  // the margins flickered between them, 7-2: they stay as they were.)
  // (Not those of the area just left: the way out of a pipe into another,
  // they put 7-5's first part's walls over its goal area.)
  if (gameplay_ && ram(0x009c) != 0 && !wallsStale_) return;
  wallsStale_ = false;
  findWallsIn();
  int width = (ram(0x0022) + 1) * 256;
  // (Thin walls too, each on its own, the other kept, the room not centred:
  // the W8 fortress's hall of doors, 14 wide between walls of one column,
  // its neighbours came into view only as the camera moved, the margins
  // walled. 2-1's bonus room at the level's start keeps its left.)
  bool cancel = false, thinCancel = false;
  for (int side = 0; side < 2; side++) {
    Beyond& z = beyond_[side];
    if (!z.on) continue;
    bool past;
    if (ram(0x1eb8) != 0) {  // (held still: it is where it is)
      int cam = ram(0x0210) | ram(0x0211) << 8;
      past = side ? cam + 256 > z.edge : cam < z.edge;
    } else if (side == 0) {
      int cam = std::max(0, z.show - 0x70);
      past = cam < z.edge;
    } else {
      int cam = std::min(width - 256, z.show - 16 - 0x80);
      past = cam + 256 > z.edge;
    }
    if (!past) continue;
    if (z.tile < 0) {
      cancel = true;
    } else {
      z = Beyond{};
      thinCancel = true;
    }
  }
  if (cancel) {
    beyond_[0] = beyond_[1] = Beyond{};
    boxed_ = false;
  }
  if (thinCancel) boxed_ = false;
}

void Smb3Engine::startFrameView(Ppu& ppu) {
  // Called before the first line: by now this frame's NMI has set the camera.
  int prevAdj[3] = {0, bgAdj_[1], bgAdj_[2]};
  bgAdj_[1] = bgAdj_[2] = 0;
  if (ext_ == 0 || spade_) {
    speedPrevOk_ = false;
    viewInit_ = false;
    if (spade_) ppu.setExt(extL_ = ext_, extR_ = ext_);
    return;
  }
  cam16_ = ram(0x0210) | (ram(0x0211) << 8);
  int camS = cam16_ & 0x8000 ? cam16_ - 0x10000 : cam16_;
  if (gameplay_ && !vertical_ && !setPiece_) planFill();
  // An end piece carried on (see fetchWideTile) in the game's own 256 px.
  endPiece_ = false;
  if (gameplay_ && !vertical_ && !setPiece_) {
    int width = (ram(0x0022) + 1) * 256;
    endPiece_ = camS < 0 || camS + 256 > width;
    for (int side = 0; side < 2; side++) {
      if (side ? camS + 256 < width - 16 : camS > 0) continue;
      for (int row = 0; row < kFillRows; row++) endPiece_ |= carriesEnd(side, row);
    }
  }
  // A set piece with a wall at its edge, half its height in view or more (the
  // map pipes' caves, earth both sides; 5-3's start, its block wall; the hand
  // traps' rooms): walled in both sides, of the room's own blocks (the
  // commonest tile in 2 x 2 or more of itself), as the player can't leave
  // there; not black. Rooms open at the edges (the hammer brothers', the
  // bonus rooms) stay black: made up, their blocks came out a second time.
  // (Decided once as the room is entered and kept for it, until the screen
  // goes dark for the next: from the rows in view, or if they don't show it
  // walled, the whole room's. Decided afresh each frame from the rows in
  // view, a tall pipe room flying up and down went walled, open, walled, its
  // sides bricks, then stripes carried on from its edges, then checks;
  // entered at its foot, it was open there, its own pattern carried on as
  // Mario flew up. From the whole room alone, a map pipe's cave, earth only
  // round the rows in view, wasn't walled at all.)
  const bool roomDark = ppu.forcedBlank() || ppu.brightness() == 0;
  if (!gameplay_ || !setPiece_ || roomDark) pieceLatched_ = pieceOpenDone_ = false;
  pieceWall_ = -1;
  if (gameplay_ && setPiece_ && pieceLatched_) {
    pieceWall_ = pieceWallLatch_;
  } else if (gameplay_ && setPiece_) {
    pieceLatched_ = !roomDark;
    auto meta = [&](int c, int row) { return tileMeta((c >> 4) * 0x1b0 + row * 16 + (c & 15)); };
    // (walled over these rows: the commonest wall tile, -1 none; -2 not walled)
    auto wallOver = [&](int r0, int r1) {
      bool walled = false;
      for (int c : {0, 15}) {
        int n = 0;
        for (int row = r0; row <= r1; row++) n += hardTile(meta(c, row));
        walled |= 2 * n >= r1 - r0 + 1;
      }
      if (!walled) return -2;
      std::map<int, int> inner;
      for (int c = 0; c < 16; c++)
        for (int row = r0; row <= r1; row++) {
          int t = meta(c, row);
          if (!hardTile(t) || isPipe(t) || isItem(t)) continue;  // (not its pipes, nor its bricks)
          if (c < 15 && row < kFillRows - 1 && meta(c + 1, row) == t && meta(c, row + 1) == t && meta(c + 1, row + 1) == t)
            inner[t]++;
        }
      // (none: black, as a frame's edge carried on came out in stripes, 2-6's
      // bonus room's: one tile thick)
      int most = 0, wall = -1;
      for (auto& kv : inner)
        if (kv.second > most) wall = kv.first, most = kv.second;
      return wall;
    };
    int camY = ram(0x0216) | ram(0x0217) << 8;
    int w = wallOver(std::clamp((camY + 8) >> 4, 0, kFillRows - 1), std::clamp((camY + 183) >> 4, 0, kFillRows - 1));
    if (w == -2) w = wallOver(0, kFillRows - 1);
    pieceWall_ = w >= 0 ? w : -1;
    pieceWallLatch_ = pieceWall_;
  }
  // A set piece open at its edges (the King's rooms, the hammer brothers',
  // the bonus rooms; not the toad house, framed): carried on, dimmed (the
  // player can't go there), row by row, only what runs right across it: a
  // pattern of 1-4 tiles from the edge that 12 of the row's 16 follow (the
  // floor, the ceiling, the wallpaper between the pillars), else the row's
  // commonest tile if half of it is (the sky, a plain wall), else nothing.
  // Made up from its edges otherwise, its blocks, palms and throne came out
  // a second time.
  pieceOpen_ = gameplay_ && setPiece_ && pieceWall_ < 0 && ram(0x070a) != 0x07;
  // A map battle: a single-page room with a hammer, boomerang, sledge or
  // fire brother in it (NES IDs $81/$82/$86/$87 in $0671+), latched for the
  // room (they are gone once beaten).
  if (!setPiece_) battleRoom_ = false;
  for (int k = 0; k <= 4 && setPiece_ && !battleRoom_; k++) {
    uint8_t id = ram(0x0671 + k);
    battleRoom_ = ram(0x0661 + k) && (id == 0x81 || id == 0x82 || id == 0x86 || id == 0x87);
  }
  // (Worked out once for the room too, as it's entered: see pieceLatched_.
  // Afresh each frame from the rows in view, a tall pipe room's sides went
  // framed and not, checks and stripes, as Mario flew up and down.)
  if (pieceOpen_ && !pieceOpenDone_) {
    pieceOpenDone_ = pieceLatched_;
    auto meta = [&](int c, int row) { return tileMeta(row * 16 + c); };
    // (The room's own wall or sky: its commonest tile the player goes
    // through and seen beside itself (a wall's, not a pillar's), for the rows
    // with nothing that runs right across: the King's room's diamonds all
    // the way up, as at its left edge, not its zigzag or plain wall by rows.)
    // (Over the rows on screen: the tile memory's rows above the King's room
    // are all one filler tile, and that came out black.)
    int camY = ram(0x0216) | ram(0x0217) << 8;
    int r0 = std::clamp((camY + 8) >> 4, 0, kFillRows - 1), r1 = std::clamp((camY + 183) >> 4, 0, kFillRows - 1);
    int fill = -1;
    {
      std::map<int, int> count;
      std::set<int> wide;
      for (int r = r0; r <= r1; r++)
        for (int c = 0; c < 16; c++) {
          int t = meta(c, r);
          if (hardTile(t) || standTile(t)) continue;
          count[t]++;
          if (c < 15 && meta(c + 1, r) == t) wide.insert(t);
        }
      int most = 0;
      for (auto& kv : count)
        if (wide.count(kv.first) && kv.second > most) fill = kv.first, most = kv.second;
    }
    // (A frame's wall tile, per side: the solid tile beside itself most
    // often in the rows on screen, if the edge column is framed in it. Or
    // rather the room's solid tile seen in a 2 x 2 of itself, anywhere in it,
    // if there is one: a tile beside itself only across is the floor's or
    // the ceiling's, an outline along one edge, and the walls came out lined.)
    int solid2x2 = -1;
    {
      std::map<int, int> blocks;
      for (int r = 0; r + 1 < kFillRows; r++)
        for (int c = 0; c < 15; c++) {
          int t = meta(c, r);
          if (!(hardTile(t) || standTile(t)) || isPipe(t) || isItem(t)) continue;
          if (meta(c + 1, r) == t && meta(c, r + 1) == t && meta(c + 1, r + 1) == t) blocks[t]++;
        }
      int most = 0;
      for (auto& kv : blocks)
        if (kv.second > most) solid2x2 = kv.first, most = kv.second;
    }
    int wallFill[2] = {-1, -1};
    for (int side = 0; side < 2; side++) {
      bool framed = false;
      for (int r = r0; r <= r1 && !framed; r++) {
        int e = meta(side ? 15 : 0, r);
        bool only = hardTile(e) || standTile(e);
        for (int q = r0; q <= r1 && only; q++)
          for (int c = 1; c < 15 && only; c++) only = meta(c, q) != e;
        framed = only;
      }
      if (!framed) continue;
      std::map<int, int> wide;
      for (int r = r0; r <= r1; r++)
        for (int c = 0; c < 15; c++) {
          int t = meta(c, r);
          if ((hardTile(t) || standTile(t)) && !isPipe(t) && !isItem(t) && meta(c + 1, r) == t) wide[t]++;
        }
      int most = 0;
      for (auto& kv : wide)
        if (kv.second > most) wallFill[side] = kv.first, most = kv.second;
      if (solid2x2 >= 0) wallFill[side] = solid2x2;
    }
    pieceFramed_ = wallFill[0] >= 0 || wallFill[1] >= 0;
    for (int side = 0; side < 2; side++)
      for (int row = 0; row < kFillRows; row++) {
        PieceRow& pr = pieceRow_[side][row];
        pr = PieceRow{};
        // (Not a pattern only near the edge: the King's room's pillars came
        // out down half its wall, or as slices above its wallpaper.)
        // (A frame drawn only in the edge column (the coin rooms' border):
        // the pattern from inside it; from the edge, the border came out
        // every 4 columns.)
        int edge = meta(side ? 15 : 0, row);
        bool frame = true;
        for (int r = r0; r <= r1 && frame; r++)
          for (int c = 1; c < 15 && frame; c++) frame = meta(c, r) != edge;
        pr.a = side ? (frame ? 14 : 15) : (frame ? 1 : 0);
        // (A frame of a solid tile (the coin rooms' checked walls): the
        // wall goes on to the display's edge, the room walled in, of its
        // border drawn to repeat side by side (the top's or the floor's: the
        // side's own has an outline on its inner edge, which came out in
        // stripes, 2-6's); and round a pipe through it, 1-3's way in.)
        if ((frame && (hardTile(edge) || standTile(edge))) || (isPipe(edge) && wallFill[side] >= 0)) {
          pr.tile = wallFill[side] >= 0 ? wallFill[side] : edge;
          continue;
        }
        for (int p = 1; p <= 4 && !pr.p; p++) {
          int n = 0, m = 0;
          for (int c = 0; c < 16; c++) {
            if (side ? c > pr.a : c < pr.a) continue;
            m++;
            n += meta(c, row) == meta(side ? pr.a - ((pr.a - c) % p) : pr.a + (c - pr.a) % p, row);
          }
          // (solid in part, blocks among the wallpaper (2-6's coin room):
          // no, they came out a second time; solid all through, a floor: yes)
          bool solid = false, open = false;
          for (int k = 0; k < p; k++) {
            int t = meta(side ? pr.a - k : pr.a + k, row);
            (hardTile(t) || standTile(t) || isItem(t) ? solid : open) = true;
          }
          if (n * 4 >= m * 3 && !(solid && open)) pr.p = p;
        }
        if (!pr.p) {
          // (A solid run of 4 or more from the edge, a floor of its own, goes
          // on: the King's room's carpet at its right. Else the room's wall or
          // sky: never a solid tile (the hammer brothers' sand block came out
          // twice), nor only the edge's (a stripe of pillars, palm leaves in
          // a row).)
          int e = meta(side ? 15 : 0, row), run = 0;
          while (run < 16 && meta(side ? 15 - run : run, row) == e) run++;
          if (run >= 4) {  // (solid or not: the carpet is drawn on, not stood on)
            pr.p = 1;
          } else {
            pr.tile = fill;  // (see above)
          }
        }
      }
  }
  // Bowser's room, at the castle's end, open there (no wall at the end
  // column, its door at the top of the steps; from the way in, not only once
  // he holds the screen ($1EB8), or the wall came in then): past the end,
  // the game's screen turned round about its middle, a room walled both
  // sides; the end's own scenery turned round put a second door and more
  // steps there. (Only there: the World 2 fortress's Boom-Boom room, the
  // same at its end, got a slice of its wall beyond its own.)
  heldMirror_[0] = heldMirror_[1] = false;
  int objs = ram(0x0031) | ram(0x0032) << 8 | ram(0x0033) << 16;
  if (gameplay_ && !vertical_ && !setPiece_ && ram(0x0727) == 7 && objs == 0x27fb7d) {
    int width = (ram(0x0022) + 1) * 256, last = width / 16 - 1;
    int camY = ram(0x0216) | ram(0x0217) << 8;
    int r0 = std::clamp((camY + 8) >> 4, 0, kFillRows - 1), r1 = std::clamp((camY + 183) >> 4, 0, kFillRows - 1);
    for (int side = 1; side < 2; side++) {  // (its end, not its start)
      int c = side ? last : 0;
      bool wall = true;
      for (int row = r0; row <= r1 && wall; row++) wall = hardTile(tileMeta((c >> 4) * 0x1b0 + row * 16 + (c & 15)));
      heldMirror_[side] = !wall;
    }
  }
  // "COURSE CLEAR!": the goal card ($41, special slots 5-7) puts the message
  // on BG3, on the main screen ($0208 bit 2), scrolled to 0,-1 with no layer
  // effect ($02BF = 0: $28:CB10). Only the 256 px the game shows hold it;
  // the rest of BG3's map keeps what the layer showed before (3-7's clouds),
  // so the margins are blank meanwhile.
  goalMsg_ = false;
  if (gameplay_ && (ram(0x0208) & 4) && ram(0x02bf) == 0 && ppu.bgHofs(2) == 0 && (ppu.bgVofs(2) & 0xff) == 0xff)
    for (int s = 5; s <= 7; s++) goalMsg_ |= ram(0x0671 + s) == 0x41;
  {
    // (A room found or left while the camera walks: the view glides there,
    // as after auto-scroll; after a jump or in the dark it is there at once.)
    bool was = boxed_;
    int wasCentre = boxCentre_;
    findWalls();
    bool dark = ppu.forcedBlank() || ppu.brightness() == 0;
    if (viewInit_ && (boxed_ != was || (boxed_ && boxCentre_ != wasCentre)) && !dark &&
        std::abs(camS - easeCam_) <= 16)
      easing_ = roomGlide_ = true;
  }
  if (gameplay_ && onePage_ && !setPiece_) planCabin(ppu);
  if (viewExt_ != ext_) autoL_ = -1;  // (the width changed)
  // The game leaves its auto-scroll flag set once the camera has reached the
  // level end; the scrolling is over then. A pause on the way (the W6
  // airship's, 7-4's: speed 0 for a while, the game's auto-scroll still on,
  // $0580) keeps the view where it is.
  bool paused = autoL_ >= 0 && ram(0x0580) != 0;
  bool scrolling = (autoscroll() || paused) && !vertical_ && camS < (ram(0x0022) + 1) * 256 - 256 - 2;
  if (scrolling) {
    // Keep the view where it is (once the camera moves: the flag is still set
    // in an area entered from an auto-scrolling one, e.g. 1-4's goal).
    if (autoL_ < 0 && ram(0x1a0e) != 0) autoL_ = viewInit_ ? extL_ : ext_;
  } else {
    if (autoL_ >= 0) easing_ = true;  // ended: back to the normal view, gently
    autoL_ = -1;
  }
  currentSplit(&extL_, &extR_);
  if (easing_ && viewInit_) {
    // Only as the camera moves (up to 3 px a frame): the picture never moves
    // on its own, e.g. backwards when the W8 tank's scroll stops. Into or out
    // of a room it goes on at 2 px a frame (it was left part way when the
    // camera stopped).
    int target = extL_, d = std::min(3, std::abs(camS - easeCam_));
    if (roomGlide_) d = std::max(d, 2);
    extL_ = shownL_ + std::clamp(target - shownL_, -d, d);
    extR_ = 2 * ext_ - extL_;
    if (extL_ == target) easing_ = roomGlide_ = false;
  }
  easeCam_ = camS;
  bool shifting = viewInit_ && extL_ != shownL_;
  shownL_ = extL_;
  bool fresh = !viewInit_;
  viewInit_ = true;
  viewExt_ = ext_;
  ppu.setExt(extL_, extR_);
  learnLayerSpeeds(ppu);
  // While the view is held at a level edge it moves differently from the
  // game's camera; shift the slower background layers to match, so they
  // keep their parallax relative to the held view.
  // A layer speed is only known once measured (the airships' sky at 1x, not
  // the usual 1/2): a new one leaves the layer where it is, and the
  // difference goes while the screen is dark, or 1 px a frame while the view
  // shifts (the layer moves differently from the 4:3 game then anyway).
  int shift = ext_ - extL_;
  bool dark = ppu.forcedBlank() || ppu.brightness() == 0;
  for (int l = 1; l <= 2; l++) {
    int target = shift ? parallaxAdjust(layerSpeed_[l], camS, shift) : 0;
    if (fresh || dark) adjOff_[l] = 0;
    else if (layerSpeed_[l] != adjSpeed_[l]) adjOff_[l] = prevAdj[l] - target;
    else if (shifting && adjOff_[l] != 0) adjOff_[l] -= adjOff_[l] > 0 ? 1 : -1;
    adjSpeed_[l] = layerSpeed_[l];
    bgAdj_[l] = (int16_t)(target + adjOff_[l]);
  }
}

// Extra scroll for a layer moving at `speed` x the camera, while the view is
// `shift` px from where the game's camera would put it: the layer as the game
// would scroll it for the view, minus as it is scrolled now (both rounded
// down, as the game does), minus the view shift itself.
int parallaxAdjust(double speed, int cam, int shift) {
  auto layerX = [&](int c) { return (int)std::floor(speed * c); };
  return layerX(cam + shift) - layerX(cam) - shift;
}

// Background layer speeds (relative to the camera) differ per level type;
// they are measured while the camera moves and kept per tileset.
void Smb3Engine::learnLayerSpeeds(Ppu& ppu) {
  int ts = ram(0x070a) & 15;
  int cam = cam16_ & 0x8000 ? cam16_ - 0x10000 : cam16_;
  if (ts != speedTileset_) {
    speedTileset_ = ts;
    for (int l = 1; l <= 2; l++) {
      layerSpeed_[l] = knownSpeed_[ts][l];
      sumCam_[l] = sumLayer_[l] = 0;
    }
  }
  // (Only while the view keeps a constant offset: a speed change then moves
  // the layer once instead of mid-transition.)
  bool centred = (extL_ == ext_ && extR_ == ext_) || autoL_ >= 0;
  for (int l = 1; l <= 2; l++) {
    int h = ppu.bgHofs(l);
    if (speedPrevOk_) {
      int dc = cam - speedPrevCam_;
      int dh = ((h - speedPrevHofs_[l]) % 1024 + 1024 + 512) % 1024 - 512;
      if (std::abs(dc) > 24) {
        sumCam_[l] = sumLayer_[l] = 0;  // camera jumped (pipe, new area)
      } else if (dc != 0) {
        sumCam_[l] += dc;
        sumLayer_[l] += dh;
        if (std::abs(sumCam_[l]) >= 64) {
          double k = (double)sumLayer_[l] / sumCam_[l];
          static const double kSpeeds[] = {0.0, 0.25, 0.5, 1.0, 2.0};
          double best = 1.0;
          for (double s : kSpeeds)
            if (std::fabs(k - s) < std::fabs(k - best)) best = s;
          knownSpeed_[ts][l] = best;
          if (centred) layerSpeed_[l] = best;  // never change it mid-transition
          sumCam_[l] = sumLayer_[l] = 0;
        }
      }
    }
    speedPrevHofs_[l] = h;
  }
  speedPrevCam_ = cam;
  speedPrevOk_ = true;
}

void Smb3Engine::beginLine(Ppu& ppu, int row) {
  ppu.objAltMask[0] |= coopMask_[0];  // (co-op: Luigi, see smb3coop.cpp)
  ppu.objAltMask[1] |= coopMask_[1];
  ppu.objAlt2Mask[0] |= coopMask2_[0];
  ppu.objAlt2Mask[1] |= coopMask2_[1];
  if (row == 0) {
    startFrameView(ppu);
    hudFrame();  // (co-op's status bar: over what the game wrote in this vblank)
    boxFrame();
    cardFrame();
  }
  // The toad house keeps its own shape, black either side of it (its black
  // is the picture's own; the backdrop colour is not).
  if (ram(0x070a) == 7) ppu.maskWide = true;
  ppu.bgHofsAdj[1] = bgAdj_[1];
  ppu.bgHofsAdj[2] = bgAdj_[2];
  line0Cam_ = cam16_ & 0x8000 ? cam16_ - 0x10000 : cam16_;
  if (extL_ == 0 && extR_ == 0) {
    // (the self-check also runs at 4:3, on lines scrolled with the camera)
    if (verifyArmed_ && row < kHudRow && gameplay_ && !vertical_ && ((ppu.bgHofs(0) - cam16_) & 0x3ff) == 0)
      verifyLine(ppu, row);
    return;
  }
  if (row < kHudRow && gameplay_ && !vertical_) {
    // The playfield on this line as the game scrolls it. Levels with an extra
    // raster split (NMI mode $A0) scroll the band below it from an IRQ, with
    // the camera this frame's logic has just moved, one frame ahead of the
    // rest (World 1 fortress's moving floor, 3-2, the tanks). The margins
    // follow the line's own scroll, or they slipped against the 4:3 picture
    // there.
    int d = (ppu.bgHofs(0) - cam16_) & 0x3ff;
    if (d >= 0x200) d -= 0x400;
    line0Cam_ += d;
    if (verifyArmed_ && ppu.bgHofsAdj[0] == 0) verifyLine(ppu, row);
  }
  if (row < kHudRow && gameplay_ && ram(0x037a) != 0) {
    // Paused: the menu's box (a window) and its text (see kOamUpload) are
    // centred on the display.
    ppu.winShift = (extR_ - extL_) / 2;
  }
  if (irisR_ >= 0 && row < kHudRow) {
    int dy = row - kIrisY, d = irisR_ * irisR_ - dy * dy;
    int hw = d >= 0 ? (int)std::sqrt((double)d) : -1;
    ppu.wideShowLo = kIrisX - hw;
    ppu.wideShowHi = kIrisX + hw + 1;
  }
  if (row < kHudRow && endPiece_) ppu.wideFetchAll[0] = true;  // (see fetchWideTile)
  if (row < kHudRow && gameplay_ && ((beyond_[0].on && beyond_[0].from > line0Cam_) ||
                                     (beyond_[1].on && beyond_[1].from < line0Cam_ + 256)))
    ppu.wideFetchAll[0] = true;  // (a wall's edge column, see fetchWideTile)
  if (row < kHudRow && gameplay_) {
    // Beyond a wall (see findWalls) the margins show more wall, so what moves
    // there isn't drawn (2-1's next pipe room's Paratroopa over the wall).
    if (beyond_[0].on) ppu.objShowLo = std::min(0, beyond_[0].show - line0Cam_);
    if (beyond_[1].on) ppu.objShowHi = std::max(256, beyond_[1].show - line0Cam_);
  }
  // A vertical area wraps round (the player leaving one side comes back in
  // at the other): its margins, the scenery only (see fetchWideTile), are
  // dimmed, as SMB2's (an option).
  if (row < kHudRow && gameplay_ && vertical_ && dimWrap_) ppu.dimLeft = ppu.dimRight = 96;
  if (row < kHudRow && gameplay_ && setPiece_) {
    // A single-page room (the bonus rooms, the hammer brothers' and the
    // King's) is a set piece: black either side, everything. (Made up from
    // its edges it showed what it doesn't have: the hammer brothers' blocks a
    // second time, a second throne.) The airships' boss rooms carry on as the
    // levels do: the night sky round the cabin, the deck's posts. A walled
    // one is walled in (see startFrameView), with nothing drawn in the walls.
    // (Not the princess's letter: the King's room's tile rows above it (0-14,
    // black round a frame of $47-$51), the camera taken up to them. Black
    // either side, as at 4:3; carried on, its paper came out white to the
    // display's edges.)
    int camY = ram(0x0216) | ram(0x0217) << 8;
    bool letter = ram(0x070a) == 2 && ((camY + row) >> 4) < 15 && tileMeta(2 * 16 + 2) == 0x47;
    if ((pieceWall_ >= 0 || pieceOpen_) && !letter) {
      ppu.objShowLo = std::max(ppu.objShowLo, -line0Cam_);
      ppu.objShowHi = std::min(ppu.objShowHi, 256 - line0Cam_);
      // (carried on: see startFrameView; dimmed only in a map battle (see
      // battleRoom_), the player shut in with the brothers; not the King's
      // room, the coin rooms or other rooms, carried on as they are)
      if (pieceOpen_ && dimWrap_ && battleRoom_) ppu.dimLeft = ppu.dimRight = 96;
    } else {
      ppu.wideShowLo = std::max(ppu.wideShowLo, -line0Cam_);
      ppu.wideShowHi = std::min(ppu.wideShowHi, 256 - line0Cam_);
    }
  }
  if (spade_ && row < kHudRow) {
    // The Spade game's board stays the 4:3 one, centred, black either side
    // (as the toad house). (With its background's pattern carried on round
    // it, the strips, shifted part of the way on the high frame rate's
    // in-between pictures, slid out over the pattern.) Its strips jump and
    // wrap round: not moved on the in-between pictures (a line's shift
    // dragged the status bar's edge out into the board's black border).
    ppu.maskWide = true;
  }
  if (spade_) ppu.subStill = true;
  if (row >= kHudRow) {
    // Status bar centred on the display (it stays put while the view is held
    // at a level edge); the sides repeat its filler column (tile column 0).
    PpuLayerRemap& rm = ppu.layerRemap[0];
    int c0 = (256 + extR_ - extL_) / 2 - 128;
    rm.count = 3;
    rm.seg[0] = {(int16_t)c0, (int16_t)(c0 + 256), 0, 0};
    rm.seg[1] = {(int16_t)-extL_, (int16_t)c0, 0, 8};
    rm.seg[2] = {(int16_t)(c0 + 256), (int16_t)(256 + extR_), 0, 8};
  }
}

// Development self-check (smastool verifywide): on every playfield line of
// the next frame, the playfield as the margins would draw it (level RAM, at
// the line's scroll) against the game's own tilemap, across the 4:3 part.
// Tiles count as matching when they draw the same pixels (the game blanks a
// collected coin with palette 0, its table's blank has palette 5).
std::string Smb3Engine::verifyLines() {
  char buf[160];
  snprintf(buf, sizeof buf, "layer 1: %d compared, %d mismatched (rows %d-%d, first at level x %d: %04X for %04X)",
           verifyCount_, verifyBad_, verifyBad_ ? verifyRowLo_ : 0, verifyBad_ ? verifyRowHi_ : 0,
           verifyBad_ ? verifyX_ : 0, verifyRam_, verifyVram_);
  std::string s = buf;
  for (const VerifyTile& t : verifyTiles_) {
    snprintf(buf, sizeof buf, "\n  tile at level %d,%d metatile %03X: %04X for %04X", t.x, t.y, t.m, t.ram, t.vram);
    s += buf;
  }
  verifyTiles_.clear();
  verifyArmed_ = true;
  verifyCount_ = verifyBad_ = 0;
  verifyRowLo_ = 999;
  verifyRowHi_ = -1;
  return s;
}

// Whether two BG1 tilemap words draw the same 8x8 pixels (colours, with
// transparency; priority only matters where something is drawn).
static bool sameLook(const Ppu& ppu, uint16_t a, uint16_t b) {
  int bpp = ppu.bgBpp(0);
  if (bpp != 2 && bpp != 4) return false;
  auto px = [&](uint16_t e, int x, int y) {
    if (e & 0x4000) x = 7 - x;
    if (e & 0x8000) y = 7 - y;
    uint16_t base = ppu.bgCharBase(0) + (e & 0x3ff) * bpp * 4;
    uint16_t p01 = ppu.vram[(base + y) & 0x7fff];
    int c = ((p01 >> (7 - x)) & 1) | ((p01 >> (14 - x)) & 2);
    if (bpp == 4) {
      uint16_t p23 = ppu.vram[(base + 8 + y) & 0x7fff];
      c |= (((p23 >> (7 - x)) & 1) | ((p23 >> (14 - x)) & 2)) << 2;
    }
    return c == 0 ? -1 : (int)ppu.cgram[(((e >> 10) & 7) << bpp) + c] | ((e & 0x2000) << 3);
  };
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 8; x++)
      if (px(a, x, y) != px(b, x, y)) return false;
  return true;
}

void Smb3Engine::verifyLine(Ppu& ppu, int row) {
  int hofs = ppu.bgHofs(0), ty = ((row + 1 + ppu.bgVofs(0)) & 0x3ff) >> 3;
  for (int sx = 0; sx < 256; sx += 8) {
    int tx = ((sx + hofs) & 0x3ff) >> 3;
    uint16_t e = 0;
    if (fetchWideTile(ppu, 0, tx, ty, sx, &e) != PpuHooks::kWideEntry) continue;
    verifyCount_++;
    if (e != ppu.tileEntry(0, tx, ty) && !sameLook(ppu, e, ppu.tileEntry(0, tx, ty))) {
      if (!verifyBad_) {
        verifyX_ = line0Cam_ + sx;
        verifyRam_ = e;
        verifyVram_ = ppu.tileEntry(0, tx, ty);
      }
      verifyBad_++;
      verifyRowLo_ = std::min(verifyRowLo_, row);
      verifyRowHi_ = std::max(verifyRowHi_, row);
      int x = (line0Cam_ + sx) & ~7, y = (ty & 63) * 8;
      bool seen = false;
      for (const VerifyTile& t : verifyTiles_) seen |= t.x == x && t.y == y;
      if (!seen && verifyTiles_.size() < 12) {
        int col = x >> 4;
        int m = tileMeta((col >> 4) * 0x1b0 + (y >> 4) * 16 + (col & 15));
        verifyTiles_.push_back({x, y, m, e, ppu.tileEntry(0, tx, ty)});
      }
    }
  }
}

// How a level carries on past its ends, per 16 px row and end. A row that
// repeats by 1-4 columns all along the 8 next to the end (or the 8 after its
// end piece) carries on as it repeats, in the bands of such rows at the
// bottom and the top: floors, ground, ceilings, sky, a checkered floor's 2.
// Between them: if the end is plain there (its first columns repeat all the
// way up: the sky, the light strip past a goal) it carries on the same way,
// else it is mirrored about the end, whole, the scenery turned round. A wall
// at the end carries on as a wall. (Carrying on the edge column made a slope a row of teeth at 1-5's
// start, a floor's end piece a row of separate bricks; deciding per row broke
// posts between rows.)
// Solid all round: the game's thresholds per tile quarter ($1E9A-$1E9D;
// $1E96-$1E99 are those for standing on). A tile in the high page stands for
// the one its tileset's table gives, $21:AB57 + 2 * tileset, as the game
// reads it ($20:A01D).
bool Smb3Engine::hardTile(int t) const {
  int b = t & 0xff;
  if (t & 0x100) b = snes_.peek(0x210000 | ((snes_.peek16(0x21ab57 + 2 * ram(0x070a)) + b) & 0xffff));
  return b >= ram(0x1e9a + (b >> 6));
}

bool Smb3Engine::standTile(int t) const {
  int b = t & 0xff;
  if (t & 0x100) b = snes_.peek(0x210000 | ((snes_.peek16(0x21ab57 + 2 * ram(0x070a)) + b) & 0xffff));
  return b >= ram(0x1e96 + (b >> 6));
}

void Smb3Engine::planFill() {
  int width = (ram(0x0022) + 1) * 256, last = width / 16 - 1;
  // (How often each metatile occurs in the level: see rareCol_.)
  std::fill(std::begin(metaCount_), std::end(metaCount_), (uint16_t)0);
  for (int c = 0; c <= last; c++)
    for (int row = 0; row < kFillRows; row++) {
      uint16_t& n = metaCount_[tileMeta((c >> 4) * 0x1b0 + row * 16 + (c & 15)) & 0x1ff];
      if (n < 0xffff) n++;
    }
  for (int side = 0; side < 2; side++) {
    auto meta = [&](int row, int k) {  // (k columns in from the end)
      int c = side == 0 ? k : last - k;
      return tileMeta((c >> 4) * 0x1b0 + row * 16 + (c & 15));
    };
    FillRow per[kFillRows];
    for (int row = 0; row < kFillRows; row++) {
      per[row] = FillRow{};
      for (int o = 0; o <= 1 && !per[row].p; o++)
        for (int q = 1; q <= 4 && !per[row].p; q++) {
          bool rep = true;
          for (int k = o; k + q < o + 8 && rep; k++) rep = meta(row, k) == meta(row, k + q);
          if (rep) per[row] = FillRow{(int8_t)q, (int8_t)o};
        }
    }
    // A wall at the end: the same tile, as thick (1-4 columns) for 3 rows or
    // more, joined to a row of nothing but it above or below (a ceiling, a
    // floor). It carries on as that wall; taken for a floor's end piece, the
    // room inside it came out beyond it (World 1 fortress's boss room, open
    // past its wall). A hill's side changes thickness down its slope.
    auto run = [&](int row) {
      int t = meta(row, 0), w = 1;
      while (w < 8 && meta(row, w) == t) w++;
      return w;
    };
    for (int r0 = 0; r0 < kFillRows;) {
      int t = meta(r0, 0), w = run(r0), r1 = r0 + 1;
      while (r1 < kFillRows && meta(r1, 0) == t && run(r1) == w) r1++;
      auto solid = [&](int row) { return row >= 0 && row < kFillRows && meta(row, 0) == t && run(row) == 8; };
      if (w <= 4 && r1 - r0 >= 3 && (solid(r0 - 1) || solid(r1)))
        for (int row = r0; row < r1; row++) per[row] = FillRow{1, 0};
      r0 = r1;
    }
    int top = 0, bottom = kFillRows - 1;
    while (top < kFillRows && per[top].p) top++;
    while (bottom >= top && per[bottom].p) bottom--;
    // A patterned band row that only changes where the row next to it in the
    // scenery does belongs to what stands there (the airship boss rooms'
    // post feet, under their posts): it goes with them. A checkered floor
    // changes where the scenery doesn't, and stays as it repeats.
    auto attached = [&](int r, int next) {
      if (per[r].p < 2) return false;
      for (int k = 0; k + 1 < 8; k++)
        if (meta(r, k) != meta(r, k + 1) && meta(next, k) == meta(next, k + 1)) return false;
      return true;
    };
    while (bottom + 1 < kFillRows && bottom >= top && attached(bottom + 1, bottom)) bottom++;
    while (top > 0 && top <= bottom && attached(top - 1, top)) top--;
    FillRow mid{};
    for (int q = 1; q <= 4 && !mid.p; q++) {
      bool rep = true;
      for (int row = top; row <= bottom && rep; row++)
        for (int k = 0; k < (q == 1 ? 1 : 2) && rep; k++) rep = meta(row, k) == meta(row, k + q);
      if (rep) mid = FillRow{(int8_t)q, 0};
    }
    for (int row = 0; row < kFillRows; row++) fill_[side][row] = row < top || row > bottom ? per[row] : mid;
    // A pipe lying across the end (5-4's exit pipe, into its end): it goes on
    // past it, and so does what it lies on (the cloud under it, past its end
    // piece: see carriesEnd). Mirrored, the pipe stopped short in the margin,
    // its rim left out, and the cloud's end piece turned round met its own.
    for (int row = top; row <= bottom; row++) {
      bool pipe = isPipe(meta(row, 0)) && meta(row, 0) == meta(row, 1);
      if (pipe) fill_[side][row] = FillRow{1, 0};
      int below = row + 1;
      if (pipe && below <= bottom && !isPipe(meta(below, 0)) && meta(below, 1) == meta(below, 2))
        fill_[side][below] = FillRow{1, (int8_t)(meta(below, 0) != meta(below, 1))};
    }
    // A wall at the end, solid all the way up and two columns thick (a pipe
    // through it counts): 3-5's end (wood blocks, its exit pipe, a stone
    // block), its goal area's start (studded blocks, the pipe from 3-5
    // through them), the World 4 towers of giant blocks. It carries on as
    // that wall, per row: its blocks as they repeat (its face aside, the
    // column next to the level's open space), else the whole run, block after
    // block; a pipe through it, its body. A pipe beside it is not part of it.
    // Mirrored, the open space came out beyond it (a pillar standing in
    // water), its blocks taken for coins and bricks.
    // (Its top may be one row down, the sky's top row over it: 4-2's end,
    // the giant blocks from row 1; mirrored, its pipe and blocks came out
    // twice, a second tower beside it.)
    bool fullWall = true;
    for (int row = 1; row < kFillRows && fullWall; row++)
      fullWall = hardTile(meta(row, 0)) && hardTile(meta(row, 1));
    // A platform whose own edge is at the end (3-9's start: the big bolted
    // blocks): its edge column (one tile down 3 rows or more), its inside next
    // to it, a top to stand on in that column. Mirrored, a copy stood back
    // to back with it, overlapping the blocks in front; past it is open space
    // instead. (Scenery, as the hills behind a goal, has no top to stand on:
    // mirrored as before.)
    bool platform = false;
    for (int row = top; row <= bottom && !platform; row++) platform = standTile(meta(row, 0)) && !hardTile(meta(row, 0));
    if (!fullWall && !mid.p && top <= bottom && platform) {
      int open = (int)(std::max_element(std::begin(metaCount_), std::end(metaCount_)) - std::begin(metaCount_));
      bool edge = false;
      for (int row = top, n = 0; row <= bottom && !edge; row++) {
        int a = meta(row, 0), b = meta(row, 1);
        bool here = a != open && b != open && a != b && !hardTile(a) && row > top &&
                    meta(row - 1, 0) == a && meta(row - 1, 1) == b;
        n = here ? n + 1 : 0;
        edge = n >= 2;
      }
      if (edge)
        for (int row = top; row <= bottom; row++) fill_[side][row] = FillRow{0, 0, (int16_t)open};
    }
    // A pipe next to the end, indoors (a ceiling over the scenery, a floor
    // under it, all along the 8 columns next to the end: the World 3
    // fortress's way down into its lower part), or with a thin wall at the end
    // all the way down (5-2's underground end, its exit pipe inside it).
    // Mirrored, a second pipe hung beside it; past it is a wall instead, of
    // the commonest solid tile there (the ceiling's blocks, the earth).
    bool indoors = !fullWall && !mid.p && top > 0 && bottom < kFillRows - 1;
    for (int k = 0; k < kFillCols && indoors; k++) indoors = hardTile(meta(top - 1, k)) && hardTile(meta(bottom + 1, k));
    bool endWall = !fullWall && !mid.p && top <= bottom;
    for (int row = 0; row < kFillRows && endWall; row++) endWall = hardTile(meta(row, 0));
    // (or hanging from a ceiling all along, its column solid from it down to
    // the pipe, over a bridge and lava, not a floor: the World 5 fortress's
    // section, its way in and its way out; mirrored, its top blocks hung
    // alone past it. The wall goes all the way down, into the lava, the
    // player can't go past the end anyway.)
    bool hanging = !fullWall && !mid.p && top > 0;
    for (int k = 0; k < kFillCols && hanging; k++) hanging = hardTile(meta(top - 1, k));
    if (hanging) {
      hanging = false;
      for (int k = 0; k < kFillCols && !hanging; k++) {
        int row = top;  // (the pipe may be below the scenery's rows: those under it carry on)
        while (row < kFillRows && hardTile(meta(row, k)) && !isPipe(meta(row, k))) row++;
        hanging = row > top && row < kFillRows && isPipe(meta(row, k));
      }
    }
    if (indoors || endWall || hanging) {
      bool pipe = hanging;
      std::map<int, int> count;
      for (int row = top; row <= bottom; row++)
        for (int k = 0; k < kFillCols; k++) {
          int t = meta(row, k);
          pipe |= isPipe(t);
          if (hardTile(t) && !isPipe(t)) count[t]++;
        }
      int wall = -1, most = 0;
      for (auto& kv : count)
        if (kv.second > most) wall = kv.first, most = kv.second;
      int low = bottom;
      if (hanging) {  // (down to the lava: the bottom rows of the bottom tile)
        low = kFillRows - 1;
        while (low > top && meta(low - 1, 0) == meta(kFillRows - 1, 0)) low--;
        low--;
      }
      if (pipe && wall >= 0)
        for (int row = top; row <= low; row++) fill_[side][row] = FillRow{0, 0, (int16_t)wall, true};
      // (the end column solid all the way down, a wall: what carries on past
      // its face doesn't go on through it, open space inside (6-6's
      // underground end, its cave went on past the wall), nor a ledge that
      // stops at it (4-3's end, its mossy underside a stripe across the rock))
      if (pipe && wall >= 0 && endWall)
        for (int row = 0; row < kFillRows; row++) {
          const FillRow& f = fill_[side][row];
          if (f.p && f.o == 1) fill_[side][row] = FillRow{0, 0, (int16_t)wall, true};
        }
    }
    // The start of an enclosed area (a ceiling and a floor all along the 8
    // columns next to it: 5-2's underground part): the player can't go past
    // it, so it's a wall, of the solid inside of the blocks round there (a
    // tile with the same on all four sides: 5-2's earth). Carried on, the
    // cave went on where he was stopped by nothing. Its face, next to the
    // level, is the face the walls there have the other way round (5-2's
    // mossy edge), turned round, if they have one.
    if (side == 0) startFace_ = -1;
    if (side == 0 && !fullWall) {
      auto across = [&](int row) {
        for (int k = 0; k < kFillCols; k++)
          if (!hardTile(meta(row, k))) return false;
        return true;
      };
      int ceil = 0, floor = kFillRows;
      while (ceil < kFillRows && across(ceil)) ceil++;
      while (floor > ceil && across(floor - 1)) floor--;
      if (ceil > 0 && floor < kFillRows && ceil < floor) {
        // (the floor where it meets the wall: the end column's, from the
        // bottom up, its top one to stand on; next to it the floor may slope)
        while (floor > ceil && standTile(meta(floor - 1, 0))) floor--;
        std::map<int, int> inner;
        for (int k = 1; k < 2 * kFillCols - 1; k++)
          for (int row = 1; row < kFillRows - 1; row++) {
            int t = meta(row, k);
            if (hardTile(t) && !isPipe(t) && meta(row, k - 1) == t && meta(row, k + 1) == t && meta(row - 1, k) == t &&
                meta(row + 1, k) == t)
              inner[t]++;
          }
        int wall = -1, most = 0;
        for (auto& kv : inner)
          if (kv.second > most) wall = kv.first, most = kv.second;
        // (only where the walls about it have a face, to finish this one with:
        // a fortress's hall, plain blocks over it, a ceiling of spikes, goes
        // on as the hall, World 6's third fortress)
        std::map<int, int> faces;  // (solid, open to its left, the wall's inside to its right)
        for (int k = 1; k < 2 * kFillCols - 1 && wall >= 0; k++)
          for (int row = ceil; row < floor; row++) {
            int t = meta(row, k);
            if (t != wall && hardTile(t) && !isPipe(t) && !hardTile(meta(row, k - 1)) && meta(row, k + 1) == wall)
              faces[t]++;
          }
        most = 2;
        for (auto& kv : faces)
          if (kv.second > most) startFace_ = (int16_t)kv.first, most = kv.second;
        if (wall >= 0 && startFace_ >= 0) {
          for (int row = ceil; row < floor; row++) fill_[side][row] = FillRow{0, 0, (int16_t)wall, true};
          // (under it, rows to be mirrored, and a floor's top to stand on:
          // 5-2's, carried on, ran on through the wall; solid floors, as the
          // fortresses' checkered ones, go on under it)
          for (int row = floor; row < kFillRows; row++)
            if ((!fill_[side][row].p && fill_[side][row].tile < 0) || !hardTile(meta(row, 0)))
              fill_[side][row] = FillRow{0, 0, (int16_t)wall, true};
          faceTop_ = ceil;
          faceBottom_ = floor - 1;
          // (and where it meets the floor, the corner the walls there have
          // the other way round, their face curving into the floor: the tile
          // under a face, with the floor's top beside it)
          startCorner_ = -1;
          if (startFace_ >= 0) {
            std::map<int, int> corners;
            for (int c = 1; c < width / 16; c++)
              for (int row = 1; row < kFillRows; row++) {
                auto at = [&](int cc, int rr) { return tileMeta((cc >> 4) * 0x1b0 + rr * 16 + (cc & 15)); };
                if (at(c, row - 1) == startFace_ && at(c - 1, row) == meta(floor, 0) && at(c, row) != startFace_)
                  corners[at(c, row)]++;
              }
            most = 0;
            for (auto& kv : corners)
              if (kv.second > most) startCorner_ = (int16_t)kv.first, most = kv.second;
          }
        }
      }
    }
    if (fullWall && !mid.p)
      for (int row = top; row <= bottom; row++) {
        int r = 1;
        if (!isPipe(meta(row, 0)))
          while (r < 8 && hardTile(meta(row, r)) && !isPipe(meta(row, r))) r++;
        auto period = [&](int n) {  // the shortest run of 1-4 repeating in the first n
          for (int q = 1; q <= 4 && q < n; q++) {
            bool rep = true;
            for (int k = 0; k + q < n && rep; k++) rep = meta(row, k) == meta(row, k + q);
            if (rep) return q;
          }
          return 0;
        };
        int p = r == 1 ? 1 : period(r);
        if (!p && r > 2) p = period(r - 1);
        if (!p && r == 2) {
          // (its inside and its face, the inside a tile that lies next to
          // itself in the level: the inside; repeated whole, the face made
          // stripes, 6-9's start. Not a giant block's two halves, World 4's.)
          int t = meta(row, 0);
          for (int c = 0; c < last && !p; c++)
            for (int rr = 0; rr < kFillRows && !p; rr++)
              if (tileMeta((c >> 4) * 0x1b0 + rr * 16 + (c & 15)) == t &&
                  tileMeta(((c + 1) >> 4) * 0x1b0 + rr * 16 + ((c + 1) & 15)) == t)
                p = 1;
        }
        fill_[side][row] = FillRow{(int8_t)(p ? p : r), 0, -1, true};
      }
    // A cliff at the end: its end column solid from the ground up, with open
    // space next to it inside the level (a single column, its face) for 3
    // rows or more above the ground. Beyond it is more of that ground, from
    // its top down: mirrored, the face came out as a thin wall with the level
    // turned round past it (2-2's end).
    {
      int run0 = kFillRows;
      while (run0 > 0 && hardTile(meta(run0 - 1, 0))) run0--;
      bool face = run0 + 3 <= bottom + 1;
      for (int row = run0; row < run0 + 3 && face; row++) face = !hardTile(meta(row, 1));
      int ground = meta(kFillRows - 1, 0);
      if (face && hardTile(ground))
        for (int row = run0; row < kFillRows; row++) fill_[side][row] = FillRow{0, 0, (int16_t)ground};
    }
    // An airship's (or a tank's, the battleship's) stern: past it is the sky
    // (the ground below, a band, carries on). Turned round, a second stern
    // stood back to back with it.
    if (side == 1 && ram(0x070a) == 0x0a && !onePage_ && top <= bottom) {  // (not its boss room: see planCabin)
      int open = (int)(std::max_element(std::begin(metaCount_), std::end(metaCount_)) - std::begin(metaCount_));
      for (int row = top; row <= bottom; row++) fill_[side][row] = FillRow{0, 0, (int16_t)open};
    }
    // A whole platform at the end, its end piece in the end column, its other
    // end in view (6-3's start: a snow-topped one on posts): past it is open
    // space. Mirrored, a second one stood back to back with it.
    if (!fullWall && !mid.p && top <= bottom) {
      int open = (int)(std::max_element(std::begin(metaCount_), std::end(metaCount_)) - std::begin(metaCount_));
      bool whole = false;
      for (int row = std::max(top, 1); row <= bottom && !whole; row++) {
        int a = meta(row, 0), b = meta(row, 1);
        if (a == b || a == open || b != meta(row, 2) || !standTile(a) || !standTile(b) || standTile(meta(row - 1, 0))) continue;
        int j = 2;
        while (j < kFillCols - 1 && meta(row, j) == b) j++;
        int c = meta(row, j);
        whole = c != b && c != open && standTile(c) && !standTile(meta(row, j + 1)) && !standTile(meta(row - 1, j));
        // (floating: open below it, not ground down to the bottom, as 7-2's
        // big blocks)
        bool below = false;
        for (int r = row + 1; r < kFillRows && !below; r++) below = !standTile(meta(r, 0)) || !standTile(meta(r, 1));
        // (or a slab of two rows at most at the bottom, each ending there:
        // 6-4's start)
        if (!below && kFillRows - row <= 2) {
          below = true;
          for (int r = row; r < kFillRows && below; r++)
            below = meta(r, 0) != meta(r, 1) && standTile(meta(r, j)) && !standTile(meta(r, j + 1));
        }
        whole &= below;
      }
      if (whole)
        for (int row = top; row <= bottom; row++) fill_[side][row] = FillRow{0, 0, (int16_t)open};
    }
    // Scenery whose own edge is the end column (6-4's goal area: its snowy
    // pillars, the last one's right side the level's last column): it ends
    // there; past it is open space (the bands, the ground, carry on). The
    // edge is told by the same tile having open space beside it elsewhere in
    // the level. Mirrored, a second pillar stood back to back with it.
    if (!fullWall && !mid.p && top <= bottom) {
      auto at = [&](int c, int row) { return tileMeta((c >> 4) * 0x1b0 + row * 16 + (c & 15)); };
      int d = side == 0 ? -1 : 1, n = 0;  // (outwards)
      // (each row's open space: its commonest tile not to stand on, near the
      // end; a goal area's may not be the level's)
      int rowOpen[kFillRows];
      for (int row = 0; row < kFillRows; row++) {
        std::map<int, int> cnt;
        for (int k = 0; k < 2 * kFillCols; k++)
          if (!standTile(meta(row, k))) cnt[meta(row, k)]++;
        rowOpen[row] = -1;
        int most = 0;
        for (auto& kv : cnt)
          if (kv.second > most) rowOpen[row] = kv.first, most = kv.second;
      }
      bool edges = true;
      for (int row = top; row <= bottom && edges; row++) {
        int t = meta(row, 0), open = rowOpen[row];
        if (t == open) continue;
        if (standTile(t) || t == meta(row, 1)) {
          edges = false;
          break;
        }
        bool seen = false;
        for (int c = 1; c < last && !seen; c++)
          for (int r = top; r <= bottom && !seen; r++) seen = at(c, r) == t && at(c + d, r) == rowOpen[r];
        edges = seen;
        n++;
      }
      if (edges && n >= 3)
        for (int row = top; row <= bottom; row++)
          if (rowOpen[row] >= 0) fill_[side][row] = FillRow{0, 0, (int16_t)rowOpen[row]};
    }
    // A mass of bricks at the end, from the bottom up (the World 5 tower's
    // top, where the player comes out): carried on as bricks to the bottom,
    // its top row's battlements as they alternate. (Its bricks taken for
    // blocks, past the end only its rows of nothing but them went on, a
    // ledge two bricks deep.)
    if (!fullWall && isItem(meta(kFillRows - 1, 0)) && hardTile(meta(kFillRows - 1, 0))) {
      int t = meta(kFillRows - 1, 0), r = kFillRows - 1;
      while (r > 0 && meta(r - 1, 0) == t) r--;
      if (kFillRows - r >= 4) {
        for (int row = r + 1; row < kFillRows; row++) fill_[side][row] = FillRow{0, 0, (int16_t)t, true};
        bool alt = meta(r, 1) != t && meta(r, 2) == t && !hardTile(meta(r, 1));
        fill_[side][r] = alt ? FillRow{2, 0, -1, true} : FillRow{0, 0, (int16_t)t, true};
      }
    }
    // The start of a hall under a ceiling of blocks all along, with lava all
    // along its bottom, no floor (World 5's second fortress): walled off as
    // an enclosed start is, of the ceiling's blocks, down into the lava.
    if (side == 0 && !fullWall && startFace_ < 0) {
      int open = (int)(std::max_element(std::begin(metaCount_), std::end(metaCount_)) - std::begin(metaCount_));
      auto across = [&](int row) {
        for (int k = 0; k < kFillCols; k++)
          if (!hardTile(meta(row, k))) return false;
        return true;
      };
      auto lavaRow = [&](int row) {  // (the same all along, not solid, not the level's open space)
        int t = meta(row, 0);
        if (hardTile(t) || t == open) return false;
        for (int k = 1; k < kFillCols; k++)
          if (meta(row, k) != t) return false;
        return true;
      };
      int lava = kFillRows;
      while (lava > kFillRows - 3 && lavaRow(lava - 1)) lava--;
      int ceil = lava - 1;
      while (ceil > 0 && !across(ceil)) ceil--;
      if (lava < kFillRows && ceil > 0 && lava - ceil > 3 && hardTile(meta(ceil, 0)))
        for (int row = ceil + 1; row < lava; row++) fill_[side][row] = FillRow{0, 0, (int16_t)meta(ceil, 0), true};
    }
    // A wall in the end column through a room's rows, under blocks solid
    // all along (7-2's pipe area: a pillar of blocks at its start, its brick
    // room beside it): past it, those rows are the blocks above carried on
    // down, as they repeat. Mirrored, the room came out beyond the wall;
    // carried on, the pillar's narrow blocks were a row of slats.
    {
      auto across = [&](int row) {
        for (int k = 0; k < kFillCols; k++)
          if (!hardTile(meta(row, k))) return false;
        return true;
      };
      for (int r = 1; r < kFillRows;) {
        if (!hardTile(meta(r, 0)) || !hardTile(meta(r, 1)) || across(r) || !across(r - 1)) {  // (two thick, not 2-5's post)
          r++;
          continue;
        }
        int a = r, b = r;
        while (b + 1 < kFillRows && hardTile(meta(b + 1, 0)) && hardTile(meta(b + 1, 1)) && !across(b + 1)) b++;
        int c0 = a;
        while (c0 > 0 && across(c0 - 1)) c0--;
        int run = a - c0, p = 0;
        for (int q = 1; q <= 4 && !p && 2 * q <= run; q++) {
          bool rep = true;
          for (int i = c0; i + q < a && rep; i++)
            for (int k = 0; k < kFillCols && rep; k++) rep = meta(i, k) == meta(i + q, k);
          if (rep) p = q;
        }
        if (!p) p = run;
        if (run >= 4 && b - a + 1 >= 2)  // (a mass of blocks above, not a ceiling's row or two: 2-5's start)
          for (int row = a; row <= b; row++) fill_[side][row].from = (int8_t)(a - p + (row - a) % p);
        r = b + 1;
      }
    }
    // A floor's top at the end, on solid ground carried on (7-3's start: grass
    // on earth), the scenery above it mirrored: the floor carries on as its
    // end column's top, and what stands on it mirrored (a hill's slope, the
    // bushes' feet in the grass) doesn't: the scenery's things to stand on
    // came out at the edge of the view, half a hill.
    if (!fullWall && !mid.p && bottom < kFillRows - 1 && bottom > top && fill_[side][bottom].tile < 0 &&
        !fill_[side][bottom].p && !fill_[side][bottom].wall && standTile(meta(bottom, 0)) &&
        hardTile(meta(bottom + 1, 0)) && !standTile(meta(bottom - 1, 0))) {
      fill_[side][bottom] = FillRow{1, 0};
      for (int row = top; row < bottom; row++)
        if (!fill_[side][row].p && fill_[side][row].tile < 0 && !fill_[side][row].wall) fill_[side][row].open = true;
    }
    // Each row's commonest tile near the end that isn't a coin or a block
    // (see fetchWideTile): open space in the 8 columns next to the end, else
    // in 16 (none in 8 at 3-1's end, where half a pipe was repeated instead;
    // 16 at first reached the goal's black backdrop under 3-1's goal pipe),
    // else anything in 8; -1 if there is none.
    for (int row = 0; row < kFillRows; row++) {
      int best = -1, bestN = 0;
      bool wall = true;  // (a row of nothing but them next to the end: a brick wall, kept)
      for (int k = 0; k < 8 && wall; k++) wall = isItem(meta(row, k));
      static const int kReach[3] = {8, 16, 8};
      for (int pass = 0; pass < 3 && best < 0 && !wall; pass++)
        for (int k = 0; k < kReach[pass]; k++) {
          int t = meta(row, k), n = 0;
          if (isItem(t) || (pass < 2 && hardTile(t))) continue;
          for (int j = 0; j < kReach[pass]; j++) n += meta(row, j) == t;
          if (n > bestN) best = t, bestN = n;
        }
      rowBg_[side][row] = (int16_t)best;
    }
    // Columns of the scenery with something the level has only once (a pipe's
    // rim, the goal's card frame) are not mirrored, in the rows of what it is
    // part of (up and down the column to the row's background): the World 5
    // fortress's start, the column of its pipe's rim had a gap in the bridge
    // under it.
    for (int k = 0; k < kFillCols; k++) {
      rareCol_[side][k] = false;
      int rr = top;
      for (; rr <= bottom && !rareCol_[side][k]; rr++) rareCol_[side][k] = metaCount_[meta(rr, k) & 0x1ff] <= 1;
      if (!rareCol_[side][k]) continue;
      // (across a gap of a row or two to more of what the level has little
      // of, as inside a frame: 5-9's card frame's bottom edge was left on its
      // own; not to what is common: the World 5 fortress's bridge under its
      // pipe lost its posts' balls)
      // (a pipe's rim takes its body; anything else only what is rare too:
      // 7-2's P switch took the blocks under it, which came out as water)
      bool pipeRare = isPipe(meta(rr - 1, k));
      auto part = [&](int row, int d) {
        for (int g = 0; g < 3; g++, row += d)
          if (row < top || row > bottom) return false;
          else if (meta(row, k) != rowBg_[side][row])
            return ((g == 0 && pipeRare && isPipe(meta(row, k))) || metaCount_[meta(row, k) & 0x1ff] <= 4);
        return false;
      };
      int lo = rr - 1, hi = rr - 1;
      while (lo > top && part(lo - 1, -1)) lo--;
      while (hi < bottom && part(hi + 1, 1)) hi++;
      rareLo_[side][k] = (int8_t)lo;
      rareHi_[side][k] = (int8_t)hi;
    }
  }
}

// An airship boss room's cabin, per tile row of the scenery's 32 columns: the
// longest run (8 columns or more) repeating by the shortest period, 1-4.
void Smb3Engine::planCabin(const Ppu& ppu) {
  int hx = ppu.bgHofs(1) >> 3, vy = ppu.bgVofs(1) >> 3;
  for (int r = 0; r < 32; r++) {
    auto e = [&](int c) { return ppu.tileEntry(1, hx + c, vy + r); };
    cabinP_[r] = cabinS_[r] = 0;
    for (int p = 1; p <= 4 && !cabinP_[r]; p++) {
      int run = 0, best = 0;
      for (int c = 0; c + p < 32; c++) {
        run = e(c) == e(c + p) ? run + 1 : 0;
        if (run + p >= 8 && run + p > best) {
          best = run + p;
          cabinS_[r] = (uint8_t)(c + 1 - run);
        }
      }
      if (best) cabinP_[r] = (uint8_t)p;
    }
  }
}

// A row carried on past its end piece (5-4's cloud platform's rounded end at
// its start, 1-2's ground's): the end piece is drawn as the row too, or it
// stood in the middle of the row, a seam. (Its period begins next to it.) Not
// a post or a wall at the end (the airships' boss rooms, the hand traps'):
// the end piece is as solid as the row, and the same tile above it is one
// too (a ground's edge, down from its corner), not a column's (the hand
// traps' wall's foot, in the rail's row).
bool Smb3Engine::carriesEnd(int side, int row) const {
  const FillRow& f = fill_[side][row];
  if (onePage_ || boxed_ || !f.p || f.o != 1 || f.tile >= 0 || f.wall) return false;  // (not in a room's frame)
  int last = ram(0x0022) * 16 + 15, c = side ? last : 0, b = side ? last - f.p : f.p;
  auto at = [&](int col, int r) { return tileMeta((col >> 4) * 0x1b0 + r * 16 + (col & 15)); };
  int e = at(c, row), t = at(b, row);
  if (hardTile(e) != hardTile(t) || standTile(e) != standTile(t)) return false;
  return row == 0 || at(c, row - 1) != e || carriesEnd(side, row - 1);
}

int Smb3Engine::fetchWideTile(Ppu& ppu, int layer, int tx, int ty, int screenX, uint16_t* entry) {
  if (layer == 0) {
    // Vertical areas are 256 px wide, round: the player leaving one side
    // comes back in at the other. Their other side in the margins (sprites
    // too) showed some of them twice, the player too, the view being wider
    // than they are round (5-2's pipe maze).
    if (vertical_) return PpuHooks::kWideBlank;
    // Render the playfield from the level's tile memory ($7E2000 + high page
    // $7E4000) through the tileset's metatile table.
    int y = (ty & 63) * 8;
    if (y >= kLevelHeight) return PpuHooks::kWideBlank;
    int x = line0Cam_ + screenX;  // (this line's scroll, see beginLine)
    int width = (ram(0x0022) + 1) * 256;
    if (screenX >= 0 && screenX < 256 && x >= 0 && x < width) {
      // (Asked for the game's own 256 px too (see beginLine) on lines with an
      // end piece to carry on: only that column is drawn here; and with the
      // game's camera past the level's end (3 px at 5-6's end): what is past
      // it, as in the margins, not the map's leftovers (a line of sky).)
      // (Its first or last 8 px past a wall, its camera a few px past the
      // wall's edge: the column the game left as it was (7-7's start wall, the
      // player against it: 3 px of the hidden area behind it, at 4:3 the wall
      // drawn before). The wall carries on there, as in the margin.)
      int c = x >> 4, last = width / 16 - 1;
      bool walled = (screenX < 8 && beyond_[0].on && x < beyond_[0].from) ||
                    (screenX >= 248 && beyond_[1].on && x >= beyond_[1].from);
      if (!walled && ((c != 0 && c != last) || !carriesEnd(c == 0 ? 0 : 1, y >> 4))) return PpuHooks::kWideVram;
    }
    // Past the level's ends (the view stays centred on the game's screen), as
    // planned per row by planFill: floors and ceilings carried on as they
    // repeat, the scenery between them mirrored about the end.
    bool flip = false, past = x < 0 || x >= width, keep = false;
    int forced = -1;
    int side = x < 0 ? 0 : 1;
    const Beyond* z = beyond_[0].on && x < beyond_[0].from   ? &beyond_[0]
                      : beyond_[1].on && x >= beyond_[1].from ? &beyond_[1]
                                                              : nullptr;
    int zRow = y >> 4;
    if (z && z->tile >= 0 && z->bandP[zRow]) {
      int c = x >= 0 ? x / 16 : -((15 - x) / 16), s = z->bandS[zRow], p = z->bandP[zRow];  // (see findWalls)
      x = (s + ((c - s) % p + p) % p) * 16 + (x & 15);
      past = false;
    } else if (z && z->tile >= 0) {
      forced = z->tile;
      past = false;
    } else if (z) {
      int c = x >= 0 ? x / 16 : -((15 - x) / 16);  // (see findWalls)
      x = (z->start + ((c - z->start) % z->period + z->period) % z->period) * 16 + (x & 15);
      past = false;
    } else if (past && heldMirror_[side]) {
      // (see startFrameView) The game's screen turned round about its middle:
      // the far side's walls and steps, not the end's own scenery twice.
      // (about the end screen's middle, where the camera stops: in place as it
      // comes into view, not sliding)
      int mx = (side ? 2 * (width - 128) : 256) - 1 - x, mc = mx >> 4, row = y >> 4;
      auto at = [&](int c) { return tileMeta((c >> 4) * 0x1b0 + row * 16 + (c & 15)); };
      int t = at(mc);
      bool inner = at(mc - 1) == t && at(mc + 1) == t;  // (a plain stretch: as it is)
      flip = !inner;
      x = flip ? mx : mc * 16 + (x & 15);
      past = false;
    } else if (past && setPiece_) {
      if (pieceOpen_) {  // (see startFrameView)
        int row = y >> 4;
        if (row >= kFillRows) return PpuHooks::kWideBlank;
        const PieceRow& pr = pieceRow_[side][row];
        if (pr.p) {
          int c = x >= 0 ? x / 16 : -((15 - x) / 16);
          int k = side ? pr.a - (((pr.a - c) % pr.p) + pr.p) % pr.p : pr.a + (((c - pr.a) % pr.p) + pr.p) % pr.p;
          x = k * 16 + (x & 15);
          past = false;
        } else if (pr.tile >= 0) {
          forced = pr.tile;
          past = false;
        } else {
          return PpuHooks::kWideBlank;
        }
      } else if (pieceWall_ < 0) {
        return PpuHooks::kWideBlank;  // (black: see beginLine)
      } else {
        forced = pieceWall_;  // (walled in: see startFrameView)
      }
    } else if (past) {
      int row = y >> 4, last = width / 16 - 1;
      if (row < kFillRows && fill_[side][row].from >= 0) {  // (see planFill)
        y = fill_[side][row].from * 16 + (y & 15);
        row = y >> 4;
      }
      const FillRow& f = fill_[side][row];
      auto at = [&](int c) { return tileMeta((c >> 4) * 0x1b0 + row * 16 + (c & 15)); };
      keep = f.wall;
      if (side == 0 && x >= -16 && startFace_ >= 0 && row >= faceTop_ &&
          (row <= faceBottom_ || (row == faceBottom_ + 1 && startCorner_ >= 0))) {
        forced = row <= faceBottom_ ? startFace_ : startCorner_;  // (see planFill)
        x = -16 + (15 - (x & 15));
        flip = true;
      } else if (f.tile >= 0) {
        forced = f.tile;
      } else if (f.p) {
        int c = x >= 0 ? x / 16 : -((15 - x) / 16), first = side == 0 ? f.o : last - f.o - f.p + 1;
        x = (first + ((c - first) % f.p + f.p) % f.p) * 16 + (x & 15);
        // (A pipe in what repeats, not at the end itself, is there once: the
        // row's background instead; 4-4's start, the pipe hanging from its
        // ceiling came out every 4 columns.)
        int mc = x >> 4;
        if (isPipe(at(mc)) && !isPipe(at(side == 0 ? 0 : last)) && rowBg_[side][row] >= 0) forced = rowBg_[side][row];
      } else {
        int mx = side == 0 ? -1 - x : 2 * width - 1 - x, mc = mx >> 4;
        int k = side == 0 ? mc : last - mc;  // (columns in from the end)
        if (k < kFillCols && rareCol_[side][k] && row >= rareLo_[side][k] && row <= rareHi_[side][k] &&
            rowBg_[side][row] >= 0) {
          // (what the level has only once is left out, never half of it, such
          // as a pipe without its rim: the row's background in its rows; the
          // end column there put a hill's corner afloat by 1-7's goal card)
          forced = rowBg_[side][row];
        } else if (k >= kFillCols || (rareCol_[side][k] && row >= rareLo_[side][k] && row <= rareHi_[side][k])) {
          // (a column with something the level has only once: the end's own
          // column instead, never half of it, such as a pipe without its rim)
          x = (side == 0 ? 0 : last) * 16 + (x & 15);
        } else {
          // Turned round, but not inside a plain stretch of one tile (1-5's
          // dirt turned round made a chevron down the middle), nor in an
          // airship's boss room (its posts, turned round where they stood
          // alone, were shaded the other way from their feet below).
          // (Not turned round: the column's tile as it is, not its halves
          // swapped.)
          int t = at(mc);
          bool inner = (mc == 0 || at(mc - 1) == t) && (mc == last || at(mc + 1) == t);
          flip = !inner && !onePage_;
          x = flip ? mx : mc * 16 + (x & 15);
        }
      }
    }
    int col = x >> 4;
    if (!past && !z && gameplay_ && !setPiece_ && forced < 0 && (col == 0 || col == width / 16 - 1) &&
        carriesEnd(col == 0 ? 0 : 1, y >> 4)) {
      const FillRow& f = fill_[col == 0 ? 0 : 1][y >> 4];  // (see carriesEnd)
      x = (col == 0 ? f.p : col - f.p) * 16 + (x & 15);
      col = x >> 4;
    }
    int s = col >> 4;
    if (s > 14 && forced < 0) return PpuHooks::kWideBlank;  // (a forced tile is past the 15th screen at 5-2's end)
    int m = forced >= 0 ? forced : tileMeta(s * 0x1b0 + (y >> 4) * 16 + (col & 15));
    if (past && forced < 0 && !keep && fill_[side][(y >> 4) % kFillRows].open && standTile(m) &&
        rowBg_[side][(y >> 4) % kFillRows] >= 0) {
      m = rowBg_[side][(y >> 4) % kFillRows];  // (see planFill)
      flip = false;
    }
    if (past && forced < 0 && !keep && isItem(m)) {
      // Coins and blocks are not shown past the ends a second time: what is
      // round them on the row instead (the sky). A row of nothing but them is
      // a wall of bricks (7-11's castle), and stays.
      int bg = rowBg_[side][y >> 4];
      if (bg >= 0) {
        m = bg;
        flip = false;
      }
    }
    *entry = snes_.peek16(tileTable_ + m * 8 + ((x >> 3) & 1) * 4 + ((y >> 3) & 1) * 2);
    if (flip) *entry ^= 0x4000;  // (H flip)
    return PpuHooks::kWideEntry;
  }
  if (layer == 2 && goalMsg_) return PpuHooks::kWideBlank;  // (see startFrameView)
  if (layer == 1 && onePage_ && !setPiece_ && gameplay_) {
    // An airship's boss room: its cabin (the scenery layer, a 256 px picture
    // with the night sky round it in VRAM) as wide as the view, each tile row
    // carried on by the run that repeats in it (the plain logs, the beams),
    // in step with the picture; so the deck's posts stand in front of more of
    // the cabin's wall (see planFill), not before the sky.
    int hx = ppu.bgHofs(1) >> 3, vy = ppu.bgVofs(1) >> 3, r = (ty - vy) & 63;
    int pc = (screenX + (ppu.bgHofs(1) & 7)) >> 3;
    if (screenX + (ppu.bgHofs(1) & 7) < 0) pc = -((-(screenX + (ppu.bgHofs(1) & 7)) + 7) >> 3);
    if (r >= 32 || (pc >= 0 && pc < 32)) return PpuHooks::kWideVram;
    int p = cabinP_[r], s = cabinS_[r];
    int col = p ? s + ((pc - s) % p + p) % p : (pc < 0 ? 0 : 31);
    *entry = ppu.tileEntry(1, hx + col, vy + r);
    return PpuHooks::kWideEntry;
  }
  if (layer == 1 && vertical_) {
    // Vertical areas only fill the left 256px of the scenery map: mirror it.
    *entry = ppu.tileEntry(1, (tx + 32) & 63, ty);
    return PpuHooks::kWideEntry;
  }
  return PpuHooks::kWideVram;
}

// ---------------------------------------------------------------------------
// Cheats

void Smb3Engine::applyCheats(uint32_t f) {
  noDamage_ = (f & kCheatNoDamage) != 0;
  fireCheat_ = (f & kCheatFireballs) != 0;
  if (!f) return;
  uint8_t* w = snes_.wram;
  int p = w[0x0726] & 1;
  uint8_t ts = w[0x070a], nmi = w[0x0100];
  bool onMap = ts == 0 && nmi == 0xc0;
  if ((f & kCheatLives) && nmi != 0x20 && (w[0x0736 + p] < 9 || w[0x0736 + p] == 0xff)) w[0x0736 + p] = 9;
  if (gameplay_) {
    if ((f & kCheatStar) && w[0x0553] < 0x40) w[0x0553] = 0x40;
    if ((f & kCheatFlight) && (w[0x00bb] == 3 || w[0x00bb] == 5)) w[0x056e] = 0xff;
    if (f & kCheatPMeter) w[0x0413] = 0x7f;
    coopCheats(f);
    if (f & kCheatTime) {
      w[0x05ee] = 3;
      w[0x05ef] = 0;
      w[0x05f0] = 0;
    }
    // Invincibility: the hurt routine does nothing (see kHurtGate), without
    // the star's music and touch (pits and lava still kill), and Boss Bass
    // can't swallow him (see kBassSwallow).
  }
  if (onMap) {
    if ((f & kCheatRaccoon) && w[0x0747 + p] < 3) w[0x0747 + p] = 3;
    if ((f & kCheatRaccoon) && coop_ && ram(0x072b) < 2 && w[0x0748] < 3) w[0x0748] = 3;  // (Luigi's, co-op)
    if (f & kCheatItems) {
      static const uint8_t kItems[] = {8, 3, 5, 9, 2, 6, 4, 1, 7, 0x0c, 0x0b, 0x0a, 0x0d};
      int base = p ? 0x1da3 : 0x1d80;
      int k = 0;
      for (int i = 0; i < 28; i++, k++)
        if (w[base + i] == 0) w[base + i] = kItems[k % sizeof(kItems)];
    }
  }
}

// ---------------------------------------------------------------------------
// Level select: start a new game in the chosen world (the map init reads the
// world number $0727 ~36 frames after Start on the title menu), then walk the
// map cursor onto the chosen panel and press A.

bool Smb3Engine::atTitle() const { return ram(0x0100) == 0x20; }

void Smb3Engine::requestWarp(int world, int level) {
  warpWorld_ = std::clamp(world, 0, 7);
  warpLevel_ = level;
  warpStage_ = 1;
  warpTimer_ = 0;
  message_.clear();
}

std::string Smb3Engine::takeMessage() {
  std::string m;
  m.swap(message_);
  return m;
}

bool Smb3Engine::findPanel(int level, int* y, int* xhi, int* x) const {
  uint8_t want;
  int nth = 0;
  if (level >= 1 && level <= 10) want = (uint8_t)(0x02 + level);
  else if (level >= 11 && level <= 13) {
    want = kPanelFortress;
    nth = level - 11;
  } else {
    return false;
  }
  // Map screens are 16 columns wide; panels sit on rows 17-25.
  for (int s = 0; s < 4; s++) {
    for (int col = 0; col < 16; col++) {
      for (int row = 17; row <= 25; row++) {
        int off = s * 0x1b0 + row * 16 + col;
        if (snes_.wram[0x2000 + off] != want) continue;
        if (nth-- > 0) continue;
        *y = (row - 15) * 16;
        *xhi = s;
        *x = col * 16;
        return true;
      }
    }
  }
  return false;
}

// The world as it would be on reaching the chosen panel by play: the levels
// numbered before it are cleared, and where the map's paths lead to it from
// the player's start, the fortresses (and, for a fortress, the levels) they
// reach in fewer steps too, with the locks and the like between those opened
// (World 1: 1-1 to 1-4, the fortress and its lock before 1-5). Otherwise the
// player came back from the level to a world closed off around him. (Where
// the way leads through a map pipe, the paths don't show it: only the
// numbered levels.)
// Done in the game's own terms: its clear bits ($1D00 + column, + $40 for
// Luigi; a bit per row), which the map applies whenever it is drawn again,
// so after the level. The paths are walked as the map's movement does
// ($23:8E5C): two cells a step, the cell between from the walkable lists
// ($21:8DC4 across, $21:8DD6 up and down). Toad houses and spade panels are
// left as they are.
void Smb3Engine::openWorldTo(int number, int ty, int txhi, int tx) {
  const uint8_t* w = snes_.wram;
  int p = ram(0x0726) & 1;
  auto tileAt = [&](int y, int xhi, int x) -> int {
    if (y < 0x10 || y > 0xb0 || xhi < 0 || xhi > 3) return -1;
    return w[0x2000 + xhi * 0x1b0 + 0x100 + (y - 0x10) + (x >> 4)];
  };
  uint8_t walk[2][9], special[8], opened[8];
  for (int i = 0; i < 9; i++) {
    walk[0][i] = snes_.peek(0x218dc4 + i);
    walk[1][i] = snes_.peek(0x218dd6 + i);
  }
  for (int i = 0; i < 8; i++) {
    special[i] = snes_.peek(0x21d790 + i);  // (locks, rocks, ...: what the map opens with a clear bit)
    opened[i] = snes_.peek(0x21d798 + i);
  }
  auto walkable = [&](int t, int axis) {
    for (uint8_t v : walk[axis])
      if (t == v) return true;
    return false;
  };
  auto openable = [&](int t, int axis) {
    for (int i = 0; i < 8; i++)
      if (t == special[i] && walkable(opened[i], axis)) return true;
    return false;
  };
  // Nodes: y $20-$A0, x 0-$E0 per screen, every $20 (a node index per 16 px
  // column and row is simplest).
  static constexpr int kCols = 64, kRows = 12;
  auto id = [](int y, int col) { return (y >> 4) * kCols + col; };
  int sy = ram(0x0043 + p), scol = ram(0x0045 + p) * 16 + (ram(0x0047 + p) >> 4);
  int target = id(ty, txhi * 16 + (tx >> 4));
  std::vector<int> dist(kRows * kCols, -1);
  std::vector<int> q{id(sy, scol)};
  dist[q[0]] = 0;
  for (size_t h = 0; h < q.size(); h++) {
    int n = q[h], y = (n / kCols) << 4, col = n % kCols;
    if (n == target) continue;  // (nothing beyond the chosen panel)
    static const int kDir[4][2] = {{0, 1}, {0, -1}, {1, 0}, {-1, 0}};  // (rows, columns)
    for (auto& d : kDir) {
      int my = y + d[0] * 0x10, mcol = col + d[1];
      int ny = y + d[0] * 0x20, ncol = col + d[1] * 2;
      if (mcol < 0 || ncol < 0 || ncol >= kCols || ny < 0x20 || ny > 0xa0) continue;
      int t = tileAt(my, mcol >> 4, (mcol & 15) << 4);
      if (t < 0 || !walkable(t, d[0] != 0)) continue;
      int m = id(ny, ncol);
      if (dist[m] >= 0) continue;
      dist[m] = dist[n] + 1;
      q.push_back(m);
    }
  }
  auto setBit = [&](int y, int col, bool both) {
    int bit = 0x01;
    for (int r = 0; r < 7; r++)
      if (snes_.peek(0x21e8a4 + r) == y) bit = snes_.peek(0x21e8ab + r);
    snes_.wram[0x1d00 + col + p * 0x40] |= (uint8_t)bit;
    if (both) snes_.wram[0x1d00 + col + (p ^ 1) * 0x40] |= (uint8_t)bit;
  };
  // (level n's panel is tile $02 + n)
  for (int y = 0x20; y <= 0xa0; y += 0x10)
    for (int col = 0; col < kCols; col++) {
      int t = tileAt(y, col >> 4, (col & 15) << 4);
      if (t >= 0x03 && t < 0x02 + number) setBit(y, col, false);
    }
  if (dist[target] < 0) return;
  for (int n = 0; n < kRows * kCols; n++) {
    if (dist[n] < 0 || dist[n] >= dist[target]) continue;
    int y = (n / kCols) << 4, col = n % kCols;
    int t = tileAt(y, col >> 4, (col & 15) << 4);
    // Fortresses (and $EB) for both players, as the game clears them
    // ($29:AFC9, $29:BCA6); for a fortress, the levels before it here.
    bool both = t == kPanelFortress || t == 0x6a || t == 0xeb;
    if (dist[n] > 0 && ((number == 0 && t >= 0x03 && t <= 0x0c) || both)) setBit(y, col, both);
    // A lock or the like on the way to a panel also before the chosen one.
    static const int kNext[2][2] = {{0, 1}, {1, 0}};
    for (auto& d : kNext) {
      int my = y + d[0] * 0x10, mcol = col + d[1];
      int m = id(y + d[0] * 0x20, col + d[1] * 2);
      if (m >= kRows * kCols || col + d[1] * 2 >= kCols || dist[m] < 0 || dist[m] >= dist[target]) continue;
      if (openable(tileAt(my, mcol >> 4, (mcol & 15) << 4), d[0] != 0)) setBit(my, mcol, true);
    }
  }
}

void Smb3Engine::updateWarp() {
  input_ = 0;
  // (The level is looked up after the warp is done, as the map closes; if it
  // never was, the next one entered isn't swapped.)
  if (swapPending_ && ++swapWait_ > 600) swapPending_ = 0;
  if (!warpStage_) return;
  int t = warpTimer_++;
  uint8_t nmi = ram(0x0100), ts = ram(0x070a);
  if (t > 3600) {
    warpStage_ = 0;
    message_ = "Level select timed out";
    return;
  }
  auto next = [&](int s) {
    warpStage_ = s;
    warpTimer_ = 0;
  };
  switch (warpStage_) {
    case 1:  // get to the 1P/2P menu on the title screen
      if (nmi == 0x20 && ram(0x00ac) == 3) {
        if (t > 20) next(2);
      } else if (nmi == 0x20) {
        if (t % 40 < 3) input_ = kPadStart;  // skip the curtain intro
      } else if (t > 600) {
        warpStage_ = 0;
        message_ = "Return to the SMB3 title screen to use level select";
      }
      break;
    case 2:  // new 1-player game, overriding the world number
      // ($072B is the menu's cursor only until Start: the game then makes it
      // the number of players, which a level's return to the map copies the
      // players' map positions by. Left at 0, the player came back at 0,0, off
      // the paths, and the level wasn't marked as cleared.)
      if (t == 0) snes_.wram[0x072b] = 0;
      if (t < 3) input_ = kPadStart;
      if (t < 45) snes_.wram[0x0727] = (uint8_t)warpWorld_;
      if (t > 45) next(3);
      break;
    case 3:  // wait for the map to be idle
      if (ts == 0 && nmi == 0xc0 && ram(0x0729) == 0x0d) {
        if (t > 30) {
          if (warpLevel_ == 0) {
            warpStage_ = 0;
          } else {
            int y, xhi, x;
            int p = ram(0x0726) & 1;
            // (the airship, Bowser's castle: entered from 1-1's panel, see
            // kLevelLookedUp)
            swapPending_ =
                warpLevel_ == kWarpAirship || (warpLevel_ >= kWarpExtra && warpLevel_ < kWarpExtra + kExtraCount)
                    ? warpLevel_
                    : 0;
            swapWait_ = 0;
            if (findPanel(swapPending_ ? 1 : warpLevel_, &y, &xhi, &x)) {
              if (!swapPending_) openWorldTo(warpLevel_ <= 10 ? warpLevel_ : 0, y, xhi, x);
              snes_.wram[0x0043 + p] = (uint8_t)y;
              snes_.wram[0x0045 + p] = (uint8_t)xhi;
              snes_.wram[0x0047 + p] = (uint8_t)x;
              // The map's camera ($0210/$0211, hi copied at $12) as walking
              // there would have left it: half screens at a time, the panel in
              // view, the map's last screen at the right. The level saves it
              // ($0722/$0724 + p) and the map comes back to it; left at the
              // start, the player came back on the screen he set off from, at
              // the panel's place on it, off the paths.
              int screens = 1;
              for (int s = 1; s < 4; s++)
                for (int off = 0x100; off < 0x1b0 && screens <= s; off++)
                  if (snes_.wram[0x2000 + s * 0x1b0 + off] != 0x02) screens = s + 1;
              int at = xhi * 256 + x, cam = 0;
              while (at - cam > 0xd0 && cam + 0x80 <= (screens - 1) * 256) cam += 0x80;
              snes_.wram[0x0210] = (uint8_t)cam;
              snes_.wram[0x0211] = snes_.wram[0x0012] = (uint8_t)(cam >> 8);
              next(4);
            } else {
              warpStage_ = 0;
              message_ = "That level is not on this world's map";
            }
          }
        }
      } else {
        warpTimer_ = std::min(warpTimer_, 1);
        if (t > 1200) warpStage_ = 0;
      }
      break;
    case 4: {  // enter the panel
      // Moving the cursor to another map screen scrolls the map first, and A
      // is ignored until it settles: press A whenever the map is idle, until
      // the level starts loading.
      bool idle = ts == 0 && nmi == 0xc0 && ram(0x0729) == 0x0d;
      if (!(ts == 0 && nmi == 0xc0)) {
        warpStage_ = 0;
      } else if (idle && t >= 2 && t % 30 < 4) {
        input_ = kPadA;
      } else if (t > 900) {
        warpStage_ = 0;
        message_ = "Level select could not enter the level";
      }
      break;
    }
  }
}

// ---------------------------------------------------------------------------
// Save states: the spawn range in effect (so a state loaded at another width
// catches up on the enemies it would otherwise skip), the smooth view and the
// layer-speed learning (so a load at the same settings replays exactly).

void Smb3Engine::saveExt(std::vector<uint8_t>& out) const {
  ExtWriter w{out};
  spawn_.save(out);
  w.v((uint8_t)swept_);
  w.v(held_);
  w.v(holdCaptured_);
  w.v(hold_);
  w.v(prevState_);
  w.v((uint8_t)holdInit_);
  w.v((uint8_t)adoptPending_);
  w.v(autoL_);
  w.v(shownL_);
  w.v((uint8_t)easing_);
  w.v((int)0);  // (was the view's offset before level-edge holds)
  w.v(viewExt_);
  w.v((uint8_t)viewInit_);
  w.v(layerSpeed_);
  w.v(knownSpeed_);
  w.v(speedTileset_);
  w.v(sumCam_);
  w.v(sumLayer_);
  w.v(speedPrevCam_);
  w.v(speedPrevHofs_);
  w.v((uint8_t)speedPrevOk_);
  // Sprite positions (see onWrite): those of the picture on screen, and those
  // being recorded for the next.
  w.v(snes_.ppu.objTrueX);
  for (bool b : snes_.ppu.objTrueXSet) w.v((uint8_t)b);
  w.v(oamX_);
  for (bool b : oamXSet_) w.v((uint8_t)b);
  // Parked objects and spawns to try again (see park()).
  w.v((uint8_t)parkedCount_);
  for (int i = 0; i < parkedCount_; i++) {
    w.v(parked_[i].idx);
    w.v(parked_[i].start);
  }
  w.v((uint8_t)retryCount_);
  for (int i = 0; i < retryCount_; i++) {
    w.v(retry_[i].idx);
    w.v(retry_[i].col);
    w.v(retry_[i].tries);
  }
  for (bool b : returning_) w.v((uint8_t)b);
  for (bool b : optional_) w.v((uint8_t)b);
  w.v(easeCam_);
  w.v(lastY_);
  w.v((uint8_t)levelShown_);
  w.v((uint8_t)levelLit_);
  w.v(irisR_);
  w.v(lastSpeed_);
  w.v(rampFrames_);
  for (bool b : optNormal_) w.v((uint8_t)b);
  w.v(deleteRange_);
  w.v(adjOff_);
  w.v(adjSpeed_);
  spawn_.saveCols(out);
  for (bool b : heldControl_) w.v((uint8_t)b);
  fireSave(out);  // (More Active Fireballs' own: 'F', when there are any)
  coopSave(out);
}

// `version` 6: states from before objects were timed on auto-scroll (their
// objects held in place are let go).
bool Smb3Engine::loadExt(const uint8_t* p, size_t n, int version) {
  if (n < SpawnCatchUp::kSaveSize) return false;
  ExtReader r{p + SpawnCatchUp::kSaveSize, p + n};
  bool swept, prevOk, init, easing, holdInit;
  int tileset, sumCam[3], sumLayer[3], prevCam, prevHofs[3], viewL, viewExt, autoL, shownL;
  int8_t held[8];
  bool captured[8];
  Hold holds[8];
  uint8_t prevState[8];
  double speed[3], known[16][3];
  r.b(swept);
  r.v(held);
  if (version < 7) {
    struct {
      bool releasing[8];
      bool captured[8];
      uint8_t holds[8][8];
    } old;
    r.v(old);
    for (int8_t& h : held) h = kFree;
    for (bool& c : captured) c = false;
    std::memset(holds, 0, sizeof(holds));
  } else {
    for (bool& c : captured) r.b(c);
    r.v(holds);
  }
  r.v(prevState);
  r.b(holdInit);
  bool adopt;
  r.b(adopt);
  r.v(autoL);
  r.v(shownL);
  r.b(easing);
  r.v(viewL);  // (unused)
  r.v(viewExt);
  r.b(init);
  r.v(speed);
  r.v(known);
  r.v(tileset);
  r.v(sumCam);
  r.v(sumLayer);
  r.v(prevCam);
  r.v(prevHofs);
  r.b(prevOk);
  if (!r.ok) return false;
  spawn_.load(p);
  swept_ = swept;
  for (int i = 0; i < 8; i++) {
    held_[i] = (int8_t)std::clamp<int>(held[i], kFree, kLast);
    holdCaptured_[i] = captured[i];
    hold_[i] = holds[i];
  }
  std::memcpy(prevState_, prevState, sizeof(prevState));
  holdInit_ = holdInit;
  adoptPending_ = adopt;
  autoL_ = std::clamp(autoL, -1, 224);
  shownL_ = std::clamp(shownL, 0, 224);
  easing_ = easing;
  viewExt_ = std::clamp(viewExt, 0, 112);
  viewInit_ = init;
  auto sane = [](double k) { return std::isfinite(k) && k >= 0 && k <= 2 ? k : 0.5; };
  for (int l = 0; l < 3; l++) layerSpeed_[l] = sane(speed[l]);
  for (int t = 0; t < 16; t++)
    for (int l = 0; l < 3; l++) knownSpeed_[t][l] = sane(known[t][l]);
  speedTileset_ = std::clamp(tileset, -1, 15);
  std::memcpy(sumCam_, sumCam, sizeof(sumCam));
  std::memcpy(sumLayer_, sumLayer, sizeof(sumLayer));
  speedPrevCam_ = prevCam;
  std::memcpy(speedPrevHofs_, prevHofs, sizeof(prevHofs));
  speedPrevOk_ = prevOk;
  // (Not in states from before it was kept: the positions stay cleared.)
  int16_t trueX[128], recX[128];
  bool trueSet[128], recSet[128];
  r.v(trueX);
  for (bool& b : trueSet) r.b(b);
  r.v(recX);
  for (bool& b : recSet) r.b(b);
  if (r.ok) {
    std::memcpy(snes_.ppu.objTrueX, trueX, sizeof(trueX));
    std::copy(trueSet, trueSet + 128, snes_.ppu.objTrueXSet);
    std::memcpy(oamX_, recX, sizeof(recX));
    std::copy(recSet, recSet + 128, oamXSet_);
  }
  uint8_t np = 0, nr = 0;
  Parked parked[kMaxParked];
  Retry retry[kMaxRetry];
  bool returning[8], optional[8];
  r.v(np);
  for (int i = 0; i < std::min<int>(np, kMaxParked); i++) {
    r.v(parked[i].idx);
    r.v(parked[i].start);
  }
  r.v(nr);
  for (int i = 0; i < std::min<int>(nr, kMaxRetry); i++) {
    r.v(retry[i].idx);
    r.v(retry[i].col);
    r.v(retry[i].tries);
  }
  for (bool& b : returning) r.b(b);
  for (bool& b : optional) r.b(b);
  if (r.ok && np <= kMaxParked && nr <= kMaxRetry) {
    parkedCount_ = 0;
    for (int i = 0; i < np; i++)
      if (parked[i].idx < 0x30) parked_[parkedCount_++] = parked[i];
    retryCount_ = 0;
    for (int i = 0; i < nr; i++)
      if (retry[i].idx < 0x30) retry_[retryCount_++] = retry[i];
    std::copy(returning, returning + 8, returning_);
    std::copy(optional, optional + 8, optional_);
  }
  // (Each later addition is optional on its own: states from before it lack it.)
  int easeCam = 0;
  r.v(easeCam);
  easeCam_ = r.ok ? easeCam : (ram(0x0210) | ram(0x0211) << 8);
  int16_t lastY[8];
  r.v(lastY);
  for (int i = 0; i < 8; i++)
    lastY_[i] = r.ok ? lastY[i] : (int16_t)(ram(0x0056 + i) << 8 | ram(0x0071 + i));
  bool shown, lit, optN[8];
  int iris, speed0, ramp, adjOff[3];
  int8_t delRange[5];
  double adjSpeed[3];
  r.b(shown);
  r.b(lit);
  r.v(iris);
  r.v(speed0);
  r.v(ramp);
  for (bool& b : optN) r.b(b);
  r.v(delRange);
  r.v(adjOff);
  r.v(adjSpeed);
  if (r.ok) {
    levelShown_ = shown;
    levelLit_ = lit;
    irisR_ = std::clamp(iris, -1, 1024);
    lastSpeed_ = std::clamp(speed0, -128, 127);
    rampFrames_ = std::clamp(ramp, 0, 8);
    ramping_ = rampFrames_ > 0;
    for (int i = 0; i < 8; i++) optNormal_[i] = optN[i] && optional_[i];
    for (int i = 0; i < 5; i++) deleteRange_[i] = delRange[i] == 0 || delRange[i] == 2 || delRange[i] == 4 ? delRange[i] : -1;
    for (int l = 0; l < 3; l++) {
      adjOff_[l] = std::clamp(adjOff[l], -512, 512);
      adjSpeed_[l] = std::isfinite(adjSpeed[l]) ? adjSpeed[l] : layerSpeed_[l];
    }
  } else {
    // (An older state: the scroll's pick-up is judged afresh from the next
    // change, and the fade-in latch from the game's mode.)
    lastSpeed_ = (int8_t)ram(0x1a0e);
    rampFrames_ = 0;
    ramping_ = false;
  }
  forgetHeldControls();
  if (r.ok && r.p + SpawnCatchUp::kColsSize <= r.end) {
    spawn_.loadCols(r.p);
    r.p += SpawnCatchUp::kColsSize;
    if (r.p + sizeof(heldControl_) <= r.end) {  // (a later addition)
      for (bool& b : heldControl_) b = *r.p++ != 0;
      fireLoad(r.p, r.end - r.p);
      coopLoad(r.p, r.end - r.p);
      return true;
    }
  }
  coopLoad(nullptr, 0);
  return true;
}

std::string Smb3Engine::debug() const {
  char buf[256];
  snprintf(buf, sizeof buf, "ts=%02X nmi=%02X vert=%d cam=%d ext=%d+%d warp=%d speed=%.2f/%.2f adj=%d/%d auto=%d",
           ram(0x070a), ram(0x0100), vertical_, cam16_, extL_, extR_, warpStage_, layerSpeed_[1], layerSpeed_[2],
           bgAdj_[1], bgAdj_[2], autoL_);
  std::string out = buf;
  char b2[64];
  snprintf(b2, sizeof b2, " piece=%d/%d wall=%d open=%d framed=%d", setPiece_, onePage_, pieceWall_, pieceOpen_, pieceFramed_);
  out += b2;
  snprintf(b2, sizeof b2, " parked=%d retry=%d held=", parkedCount_, retryCount_);
  out += b2;
  for (int s = 0; s < 5; s++) {
    out += char('0' + held_[s]);
    out += optional_[s] ? 'o' : '-';
  }
  for (int i = 0; i < parkedCount_; i++) {
    snprintf(b2, sizeof b2, " P%02X", parked_[i].idx);
    out += b2;
  }
  for (int i = 0; i < retryCount_; i++) {
    snprintf(b2, sizeof b2, " R%02X", retry_[i].idx);
    out += b2;
  }
  if (coop_) out += "\n  coop: " + coopDebug();
  return out;
}
