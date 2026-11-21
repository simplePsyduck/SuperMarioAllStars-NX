#include "game/smb2.h"
#include "game/stateext.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace {
// First level index of each world (ROM table $11C9B4).
const uint8_t kWorldFirstLevel[8] = {0, 3, 6, 9, 12, 15, 18, 20};
}  // namespace

Smb2Engine::Smb2Engine(Snes& snes) : snes_(snes) {}

void Smb2Engine::patch(uint32_t adr, uint8_t v) {
  if (!original_.count(adr)) original_[adr] = snes_.peek(adr);
  snes_.poke(adr, v);
}

void Smb2Engine::unpatchAll() {
  for (auto& o : original_) snes_.poke(o.first, o.second);
}

void Smb2Engine::frameStart(int want, bool active, bool hudAnchor) {
  want_ = want;
  active_ = active;
  hudAnchor_ = hudAnchor;
  // Gameplay: gameplay NMI mode, an area loaded and BG mode 1 (the death ->
  // character select fade switches to mode 2 a few frames before $0217
  // changes). Subspace (mirrored world) is 4:3, switched to and from on a
  // black screen only: $0628 turns 2 as the fade into it starts, and is
  // cleared before the fade out of it ends.
  bool wasGameplay = gameplay_;
  gameplay_ = ram(0x0100) == 0xc0 && ram(0x0217) == 0x80 && (ram(0x0775) & 7) == 1;
  // The level end (into Hawkmouth) clears $0217 before its fade: the view
  // stays as it was until the screen is black. (A safety net of 10 s.)
  bool black = snes_.ppu.forcedBlank() || snes_.ppu.brightness() == 0;
  if (wasGameplay && !gameplay_ && ext_ > 0) {
    endFade_ = true;
    endFrames_ = 0;
  }
  if (gameplay_ || black || ++endFrames_ > 600) endFade_ = false;
  horizontal_ = ram(0x00eb) != 0;
  cam16_ = horizontal_ ? (ram(0x04bf) << 8 | ram(0x04c1)) : 0;
  // Jar interiors ($0628 = 1) are single-screen rooms kept beyond the area's
  // pages, switched to and from the same way: shown wide and centred, the jar
  // standing in the area's scenery (see jarTile). Vertical areas are 4:3: one
  // screen wide, wrapping around, so the margins showed the other side of the
  // same room (its doors twice).
  bool latch = !latchInit_ || !gameplay_ || snes_.ppu.forcedBlank() || snes_.ppu.brightness() == 0;
  if (latch) {
    subspace_ = (ram(0x0628) & 0x0f) == 2;
    jar_ = (ram(0x0628) & 0x0f) == 1;
    verticalArea_ = !horizontal_;
    // (A vertical area one screen tall is a room, as a jar's: 1-2's room
    // through its jar, a jar's shape round it. Shown the same way.)
    roomJar_ = verticalArea_ && ram(0x0541) == 0 && !jar_ && !subspace_;
    // (A horizontal area one screen wide (2-3's first room) likewise: its
    // edges carried on as an open vertical area's (see openVerticalTile).)
    oneRoomH_ = !verticalArea_ && ram(0x0541) == 0 && !jar_ && !subspace_;
    findWalledVertical();
    latchInit_ = true;
  }
  // (Each frame shown, from where the player is: worked out on the black
  // frames of an area change it went by the area and position before (5-2
  // played from its start: its climb dimmed). Within the rows on screen:
  // 5-3's shaft, walled in below, its corridors through the edges above.
  // Faded in and out, as that changes.)
  if (latch) wrapSides_ = 0, dimAmt_[0] = dimAmt_[1] = 0;
  if (gameplay_ && !latch) {
    int r = playerReachesEdge();
    if (r >= 0) wrapSides_ = r;
    for (int side = 0; side < 2; side++) {
      int want = wrapSides_ & (1 << side) ? 96 : 0;
      dimAmt_[side] += std::clamp(want - dimAmt_[side], -6, 6);
    }
  }
  // Single-page areas (2-3, 4-3 and 6-3's first rooms) have nothing beside
  // the 256 px screen but the page repeated (4-3's lit door floated in the
  // margins at 21:9): 4:3, centred, like single-screen rooms elsewhere.
  bool onePage = horizontal_ && ram(0x0541) == 0;
  // Subspace (a horizontal area's): wide too, the margins the level beside
  // the screen it was entered from, turned round as that screen is (see
  // subspaceTile). (A vertical area's has no mirroring in the game: 4:3.)
  bool subWide = subspace_ && !verticalArea_;
  ext_ = (gameplay_ || endFade_) && (!subspace_ || subWide) &&
                 (jar_ || roomJar_ || oneRoomH_ || subWide || wallTile_[0] >= 0 || (verticalArea_ && openVertical()) ||
                  (!verticalArea_ && !onePage)) &&
                 want > 0
             ? std::min(want, 112)
             : 0;
  if (ext_ == 0) forgetBands();  // (new area: learn the bands again)
  viewSplit(cam16_, &extL_, &extR_);

  bool wide = ext_ > 0;  // (extR_ is 0 when the view sits at an area's right end)
  // (Vertical areas: the spawn tables are applied to rows there, and the view
  // is wider, not taller; they keep the game's own range. A jar's room is
  // beside the area's last page: its enemies stay out.)
  wideRange_ = wide && active && horizontal_ && !jar_ && !subspace_ && !oneRoomH_;
  if (wideRange_) {  // (subspace spawns nothing; a one-screen room keeps its own range)
    // Columns that jump into the spawn range unchecked (see spawncatchup.h).
    if (swept_) spawn_.assumeCovered();
    swept_ = false;
    // The despawn distances are bytes (at most $F0 past the screen edge): the
    // spawn columns stay inside them by the game's own margins (24 px right,
    // 16 px left), or an enemy spawned beyond is removed the next frame, and
    // again, at a view held 224 px to one side.
    int mR = std::min(extR_, 0xf0 - 0x30), mL = std::min(extL_, 0xf0 - 0x30);
    if (spawn_.track(mL, mR, [&](int m) { return (cam16_ + 0x118 + m) & ~15; },
                     [&](int m) { return (cam16_ - 0x20 - m) & ~15; }))
      retryN_ = 0;  // (the camera jumped: another place)
    // Spawn offsets (table $11978F: T_lo FF 18 E0, T_hi E0 01 FF).
    int right = 0x0118 + mR, left = (0xffe0 - mL) & 0xffff;
    patch(0x119790, right & 0xff);
    patch(0x119792, (right >> 8) & 0xff);
    patch(0x119791, left & 0xff);
    patch(0x119793, (left >> 8) & 0xff);
    // Despawn distances (immediates, 48px originally).
    patch(0x12a7fa, (uint8_t)std::min(0x30 + extL_, 0xf0));  // left
    patch(0x12a80a, (uint8_t)std::min(0x30 + extR_, 0xf0));  // right
  } else {
    spawn_.inactive();
    retryN_ = 0;
    for (uint32_t a : {0x119790u, 0x119791u, 0x119792u, 0x119793u, 0x12a7fau, 0x12a80au}) {
      auto it = original_.find(a);
      if (it != original_.end()) snes_.poke(a, it->second);
    }
  }
  // Objects are drawn only with screen X in [-128, 384) ($12:C049: CMP #$FF80
  // / CMP #$0180, 16-bit): at 21:9 a held view reaches 480 (or -224), and
  // enemies there went undrawn. Widen it to the view (never narrower).
  if (wide) {
    int left = std::min(-128, -(extL_ + 32)), right = std::max(384, 256 + extR_);
    patch(0x12c04f, left & 0xff);
    patch(0x12c050, (left >> 8) & 0xff);
    patch(0x12c056, right & 0xff);
    patch(0x12c057, right >> 8);
  } else {
    for (uint32_t a : {0x12c04fu, 0x12c050u, 0x12c056u, 0x12c057u}) {
      auto it = original_.find(a);
      if (it != original_.end()) snes_.poke(a, it->second);
    }
  }
  // Life meter (sprites at x=8): pin to the widened left edge.
  if (wide && hudAnchor) {
    int x = 8 - extL_;
    patch(0x13fb68, x & 0xff);
    patch(0x13fb8a, x < 0 ? 0x03 : 0x02);
  } else {
    for (uint32_t a : {0x13fb68u, 0x13fb8au}) {
      auto it = original_.find(a);
      if (it != original_.end()) snes_.poke(a, it->second);
    }
  }
  updateWarp();
}

// Enemy spawning with a widened range.
// * Area load ($12:9C3B) spawns what is already on screen by checking 24
//   columns from camera - $30; it runs before the wide range is patched in,
//   so enemies in the margins were skipped for good (4-1 at 21:9 had none for
//   its first screens). Start it further left and run more columns.
// * Each frame ($12:9D79) the game checks one column at the edge of the range
//   on the side the player moves towards (Y = $6E: 1 right, 2 left); queued
//   columns are checked there instead (see spawncatchup.h).
// * Before spawning, the game refuses an enemy within 24 px of the player
//   ($12:9EA0), but it compares only the low bytes of the X positions, i.e.
//   the distance modulo 256. The 4:3 spawn column is never ~256 px from the
//   player; a widened one usually is, and almost nothing spawned ahead at
//   21:9. With the wide range active, decide on the real distance instead.
namespace {
constexpr uint32_t kSweepStart = 0x129c3b, kSweepEnd = 0x129c63;
constexpr uint32_t kSweepLeftOp = 0x129c40, kSweepCountOp = 0x129c4d;  // SBC #$30, LDA #$17
constexpr uint32_t kSpawnCheck = 0x129d79;
constexpr uint32_t kNearPlayer = 0x129ea0;  // LDA $0F: player X lo - enemy X lo
// A level enemy found in the column: the search for a free slot (0-4), which
// gives up when all five are taken, and the column isn't checked again.
constexpr uint32_t kSpawnSlot = 0x129ddd, kSpawnFull = 0x129de6;
// A generator found in the column (types $47-$5B: Albatosses, Beezos, and
// their stops): STA $04B9 / RTS.
constexpr uint32_t kGenerator = 0x129dd9;
// 6-3's gate over the exit (area 1: sprites the All-Stars code adds, at x
// $490-$4AF) is drawn only with the player on page 4 or beyond ($15:D130:
// LDA $14 / CMP #$04 / BCC): in 4:3, as it scrolls in; the wide view showed
// its doorway open until he got close. Drawn once the view reaches it.
constexpr uint32_t kGatePage = 0x15d134;
// Wart's third column ($12:DCD9): left out when any of his columns is off
// the 4:3 screen (LDA $ED / BEQ), or past its right edge ($12:DCF7: X + $20
// carries); the ninth X bit from $ED ($12:DD37). Cut off down his right side
// at 21:9 as he stood beyond the 4:3 screen: drawn where the view shows it.
constexpr uint32_t kWartCol = 0x12dcd9, kWartCarry = 0x12dcf7, kWartHigh = 0x12dd37;
// The ending's parade: Wart carried off to the right, drawn at $12:95B6 from
// $14:$28 + $11:AA70[X] (DB $11); gone, and a burst of stars from x $120 (LDA #$20
// at $12:9572), once he is 32 px past the screen ($12:9564: CMP #$20 on page
// 1). Inside the wide view: he went from view there. Past its edge instead,
// placed by his own position (a 9-bit X beyond 384 read as the left edge).
// The stars ($12:9623): 8-bit X, drawn from $80 up, flying in from the 4:3
// screen's right edge: moved out with him, by the margin.
constexpr uint32_t kWartParadeX = 0x1295b6, kParadeExit = 0x129565, kParadeStarX = 0x129623;
// Albatoss and diving Beezo generators place their enemy at an edge of the
// 4:3 screen (Y = side: 0 left, 1 right), where it popped in inside the wide
// picture: after the X is set, move it out to the view's edge.
constexpr uint32_t kAlbatossPlaced = 0x12aa58;  // X = camera - $20 / + $100
constexpr uint32_t kBeezoPlaced = 0x12aa8d;     // X = camera + 0 / + $FF
// Enemies the level list places at the screen edge (diving Beezos,
// Albatosses): X = camera + $9793[$6E], its high byte about to be stored.
constexpr uint32_t kEdgePlaced = 0x129ff5;
constexpr uint32_t kEdgePlaced2 = 0x12b29a;  // (Albatosses: camera + $981F[type - $0B])
// Birdo spits only while on the 4:3 screen ($12:AFB5: LDA $ED (the enemy's
// off-screen bits) / AND #$0C / BNE): in a margin she stood there and never
// spat. In the view, she does (with the enemies active in full view).
constexpr uint32_t kBirdoSpit = 0x12afb5;
}  // namespace

void Smb2Engine::markExec() {
  for (uint32_t b : {0x000000u, 0x800000u})
    for (uint32_t pc : {kSweepStart, kSweepEnd, kSpawnCheck, kNearPlayer, kSpawnSlot, kSpawnFull, kGenerator, kGatePage, kWartCol, kWartCarry, kWartHigh, kWartParadeX, kParadeStarX, kAlbatossPlaced,
                        kBeezoPlaced, kEdgePlaced, kEdgePlaced2, kBirdoSpit})
      snes_.markExec(pc | b, true);
}

bool Smb2Engine::onExec(uint32_t pc) {
  pc &= 0x7fffff;
  if (pc == kSweepStart) {
    int e = std::min(want_, 112);
    if (!active_ || e <= 0 || !horizontal_ || (ram(0x0628) & 0x0f) == 2) return false;
    int cam = ram(0x04bf) << 8 | ram(0x04c1);
    // The view the new area can show: up to 2e px on either side.
    int l = std::min(cam, 2 * e), r = 2 * e;
    int left = std::min(0x30 + l, 0xff);
    int span = (left + 0x118 + r + 15) / 16;  // columns up to the right spawn edge
    patch(kSweepLeftOp, (uint8_t)left);
    patch(kSweepCountOp, (uint8_t)std::min(span, 0x7f));
    sweeping_ = true;
  } else if (pc == kSweepEnd && sweeping_) {
    sweeping_ = false;
    swept_ = true;
    for (uint32_t a : {kSweepLeftOp, kSweepCountOp})
      if (original_.count(a)) snes_.poke(a, original_[a]);
  } else if (pc == kSpawnCheck) {
    catchUpSpawn();
  } else if (pc == kSpawnSlot) {
    makeRoom();
  } else if (pc == kSpawnFull) {
    spawnRefused();
  } else if (pc == kGatePage && active_ && ext_ > 0 && cam16_ + 256 + extR_ > 0x490) {
    snes_.cpu.r.pc = (kGatePage + 2) & 0xffff;  // (past the BCC)
    return true;
  } else if ((pc == kWartCol || pc == kWartCarry || pc == kWartHigh) && active_ && ext_ > 0 && horizontal_) {
    int slot = ram(0x0012);
    if (slot < 9) {
      int rel = (ram(0x0015 + slot) << 8 | ram(0x0029 + slot)) - cam16_ + 0x20;  // (the third column's)
      if (rel > -16 - extL_ && rel < 256 + extR_) {
        CpuRegs& r = snes_.cpu.r;
        if (pc == kWartCol) {
          r.pc = 0xdce0;  // (past the test)
          return true;
        }
        if (pc == kWartCarry) {
          r.pc = 0xdcf9;  // (drawn, past the edge too)
          return true;
        }
        r.a = (r.a & 0xff00) | (rel < 0 || rel >= 256 ? 1 : 0);  // (then ORA #$02)
      }
    }
  } else if (pc == kWartParadeX) {
    CpuRegs& r = snes_.cpu.r;
    int idx = (r.y & 0x1ff) >> 2, x = (int16_t)(ram(0x0014) << 8 | ram(0x0028)) + snes_.peek(0x11aa70 + (r.x & 0xff));
    if (idx < 128) paradeX_[idx] = (int16_t)x, paradeSet_[idx] = true;
  } else if (pc == kParadeStarX && paradeExtR_ > 0) {
    CpuRegs& r = snes_.cpu.r;
    int idx = (0x70 + (r.y & 0xff)) >> 2;
    for (int k = 0; k < 2 && idx + k < 128; k++)
      paradeX_[idx + k] = (int16_t)((r.a & 0xff) + paradeExtR_), paradeSet_[idx + k] = true;
  } else if (pc == kGenerator && wideRange_ && !generatorAt43()) {
    snes_.cpu.r.pc = (kGenerator + 3) & 0xffff;  // (to the RTS: see generators)
    return true;
  } else if (pc == kBirdoSpit && active_ && ext_ > 0 && horizontal_) {
    int slot = ram(0x0012);
    if (slot < 9) {
      int rel = (ram(0x0015 + slot) << 8 | ram(0x0029 + slot)) - cam16_;
      if (rel > -16 - extL_ && rel < 256 + extR_) {
        CpuRegs& r = snes_.cpu.r;
        r.a &= 0xff00;  // (LDA $ED: none off-screen)
        r.z = true;
        r.n = false;
        r.pc = (uint16_t)(pc + 2);
        return true;
      }
    }
  } else if (pc == kNearPlayer && active_ && ext_ > 0) {
    int slot = snes_.cpu.r.x & 0xff;
    if (slot < 9) {
      int player = ram(0x0014) << 8 | ram(0x0028);
      int enemy = ram(0x0015 + slot) << 8 | ram(0x0029 + slot);
      int d = player - enemy;
      // Really further than 24 px: pass the test, keeping the side ($0F's sign).
      if (d >= 0x18 || d <= -0x18) snes_.wram[0x000f] = d < 0 ? 0x80 : 0x7f;
    }
  } else if ((pc == kAlbatossPlaced || pc == kBeezoPlaced) && active_ && ext_ > 0 && horizontal_) {
    CpuRegs& r = snes_.cpu.r;
    int slot = r.x & 0xff;
    if (slot < 9) {
      bool right = r.y & 1;
      int x = pc == kAlbatossPlaced ? (right ? cam16_ + 256 + extR_ : cam16_ - extL_ - 32)
                                    : (right ? cam16_ + 255 + extR_ : cam16_ - extL_);
      snes_.wram[0x0029 + slot] = x & 0xff;
      snes_.wram[0x0015 + slot] = (x >> 8) & 0xff;
    }
  } else if ((pc == kEdgePlaced || pc == kEdgePlaced2) && active_ && ext_ > 0 && horizontal_) {
    CpuRegs& r = snes_.cpu.r;
    int slot = r.x & 0xff;
    if (slot < 9 && r.mf) {
      int x = (r.a & 0xff) << 8 | ram(0x0029 + slot);
      int rel = x - cam16_;
      if (rel >= 0xf0 && rel < 0x110) x += extR_;  // the right edge: the view's
      else if (rel > -0x30 && rel < 0x10) x -= extL_;
      snes_.wram[0x0029 + slot] = x & 0xff;
      r.a = (r.a & 0xff00) | ((x >> 8) & 0xff);
    }
  }
  return false;
}

// Generators (types $47-$5B) are marks in the level: an Albatoss or Beezo
// generator from where the 4:3 spawn edge meets one, until it meets a stop
// (5-3's: from x $200 to $5C0). The widened edge met them 112 px early, and
// a stop passed, a step back (the left edge meeting the generator again)
// started it again in 4:3 but not wide: the left edge was beyond it already.
// What they make is placed at the view's edge anyway (see kAlbatossPlaced):
// they keep the game's own edges, checked here as the game would.
bool Smb2Engine::generatorAt43() const {
  int col = ram(0x0006) << 8 | (ram(0x0005) & 0xf0);
  int cam = ram(0x04bf) << 8 | ram(0x04c1);
  if (sweeping_) {
    int lo = (cam - 0x30) & ~15;  // (the area load's own 24 columns)
    return col >= lo && col < lo + 24 * 16;
  }
  return col == generatorCol43(cam);
}

int Smb2Engine::generatorCol43(int cam) const {
  int side = ram(0x006e);
  return side == 1 ? (cam + 0x118) & ~15 : side == 2 ? (cam - 0x20) & ~15 : -1;
}

void Smb2Engine::generators(int cam) {
  int col = generatorCol43(cam), page = col >> 8;
  if (col < 0 || page >= 10) return;
  int list = ram(0x00cd) << 8 | ram(0x00cc), y = 0;
  for (int p = 0; p < page; p++) {
    if (list + y >= 0x2000) return;
    y += snes_.wram[list + y];
  }
  if (list + y >= 0x2000) return;
  int n = snes_.wram[list + y];
  for (int k = 1; k + 1 < n && list + y + k + 1 < 0x2000; k += 2) {
    uint8_t t = snes_.wram[list + y + k], pos = snes_.wram[list + y + k + 1];
    if (t >= 0x47 && t < 0x5c && (pos & 0xf0) == (col & 0xf0)) snes_.wram[0x04b9] = t;
  }
}

void Smb2Engine::catchUpSpawn() {
  if (!active_ || ext_ == 0 || !horizontal_) return;
  int side = ram(0x006e);
  if (side != 1 && side != 2) return;
  int cam = ram(0x04bf) << 8 | ram(0x04c1);
  if (wideRange_) generators(cam);
  int natural = side == 1 ? (cam + 0x118 + extR_) & ~15 : (cam - 0x20 - extL_) & ~15;
  int col;
  if (spawn_.pending()) {
    if (!spawn_.next(side == 1, natural, &col)) return;
  } else if (!retryColumn(cam, &col)) {
    return;
  } else if (col != natural) {
    spawn_.displaced(side == 1, natural);
  }
  int t = (col - cam) & 0xffff;
  patch(0x11978f + side, t & 0xff);  // T_lo[side]
  patch(0x119791 + side, t >> 8);    // T_hi[side] (frameStart restores both)
}

// A level enemy the spawn turned away, all five slots taken ($12:9DE6), is
// never tried again: its column has gone by (5-1's waterfall: the jumping
// fish came into the wide range while the logs still held the slots, kept
// alive by it, and were never seen). Tried again here, its column checked
// in place of the usual one, while it stays in range, once a slot is free
// or one can be made (see makeRoom).
void Smb2Engine::spawnRefused() {
  if (!wideRange_) return;
  int idx = snes_.cpu.r.y & 0xff, col = ram(0x0006) << 8 | ram(0x0005);
  for (int i = 0; i < retryN_; i++)
    if (retry_[i].idx == idx) return;
  if (retryN_ < kRetryMax) retry_[retryN_++] = {idx, col};
}

bool Smb2Engine::retryColumn(int cam, int* col) {
  int list = ram(0x00cd) << 8 | ram(0x00cc);
  int lo = cam - 0x20 - extL_ - 16, hi = cam + 0x118 + extR_ + 16;
  bool room = false;
  for (int s = 0; s <= 4; s++) room |= ram(0x0051 + s) == 0;
  for (int i = 0; i < retryN_;) {
    Retry& r = retry_[i];
    // (gone: spawned since, or out of the range, as the game drops it)
    if (list + r.idx >= 0x2000 || (snes_.wram[list + r.idx] & 0x80) || r.col < lo || r.col > hi) {
      retry_[i] = retry_[--retryN_];
      continue;
    }
    if (room || canMakeRoom(r.idx, r.col)) {
      // (tried once: turned away for want of a slot again, it comes back)
      *col = r.col;
      retry_[i] = retry_[--retryN_];
      return true;
    }
    i++;
  }
  return false;
}

// The widened despawn range keeps more alive, and it filled all five slots:
// 4-1's Hawkmouth (the exit) came into range with the crystal, a Flurry, an
// Autobomb and its two fireballs taking them at 21:9, and never appeared,
// until the player went far enough back for it to come into range again.
// The 4:3 game would have removed what lies beyond its own range (camera
// -$30 .. +$12F) already: one of those gives way, removed as the game's
// despawn does ($12:A837: its "spawned" bit cleared, so it comes back in
// range as usual), unless carried or never despawned. One out of sight
// first; one still in the margins (it vanishes there) only for what the
// level needs (the Hawkmouth, crystal, key, potion, mushroom or a boss), or
// for one the 4:3 game would have by now, its column inside the game's own
// spawn range: the 4:3 picture's enemies before the margins' extras.
int Smb2Engine::slotToFree(int idx, int col) const {
  for (int s = 0; s <= 4; s++)
    if (ram(0x0051 + s) == 0) return -1;
  int list = ram(0x00cd) << 8 | ram(0x00cc);
  if (list + idx >= 0x2000) return -1;
  uint8_t type = snes_.wram[list + idx] & 0x7f;
  int cam = ram(0x04bf) << 8 | ram(0x04c1), best = -1, bestScore = 0;
  bool needed = col - cam >= -0x30 && col - cam < 0x128;
  for (uint8_t t : {0x1c, 0x1d, 0x1f, 0x21, 0x2a, 0x2c, 0x2d, 0x3d, 0x3e, 0x3f, 0x42, 0x43, 0x44})
    needed |= type == t;
  for (int s = 0; s <= 4; s++) {
    if (ram(0x00a8 + s) || (snes_.peek(0x11cc8c + ram(0x0090 + s)) & 0x08)) continue;
    int rel = (ram(0x0015 + s) << 8 | ram(0x0029 + s)) - cam;
    int d = rel < -0x30 ? -0x30 - rel : rel > 0x12f ? rel - 0x12f : 0;
    if (d == 0) continue;
    bool seen = rel > -extL_ - 32 && rel < 256 + extR_;
    if (seen && !needed) continue;
    int score = (seen ? 0 : 0x10000) + d;
    if (score > bestScore) {
      bestScore = score;
      best = s;
    }
  }
  return best;
}

bool Smb2Engine::canMakeRoom(int idx, int col) const { return slotToFree(idx, col) >= 0; }

void Smb2Engine::makeRoom() {
  if (!wideRange_) return;
  int list = ram(0x00cd) << 8 | ram(0x00cc);
  int best = slotToFree(snes_.cpu.r.y & 0xff, ram(0x0006) << 8 | ram(0x0005));
  if (best < 0) return;
  int y = ram(0x0441 + best);
  if (!(y & 0x80) && list + y < 0x2000) snes_.wram[list + y] &= 0x7f;
  snes_.wram[0x0051 + best] = 0;
  snes_.wram[0x042f + best] = 0;
}

// Save states: the spawn range in effect (a state loaded at another width
// catches up on skipped enemies) and the held-view layer rounding.
void Smb2Engine::saveExt(std::vector<uint8_t>& out) const {
  ExtWriter w{out};
  spawn_.save(out);
  w.v((uint8_t)swept_);
  w.v((uint8_t)subspace_);
  w.v(holdDev_);
  for (auto& s : holdSeen_)
    for (bool b : s) w.v((uint8_t)b);
  // (Later additions, each optional when loading.)
  spawn_.saveCols(out);
  w.v((uint8_t)endFade_);
  w.v(endFrames_);
  w.v(shownArea_);
  w.v(lastBright_);
  w.v(bandOff_);
  w.v(bandAdj_);
  w.v((uint8_t)jar_);
  w.v((uint8_t)1);  // (generators kept to the 4:3 edges: see generators)
}

bool Smb2Engine::loadExt(const uint8_t* p, size_t n) {
  if (n < SpawnCatchUp::kSaveSize) return false;
  ExtReader r{p + SpawnCatchUp::kSaveSize, p + n};
  bool swept, sub, seen[3][8];
  int dev[3][8];
  r.b(swept);
  r.b(sub);
  r.v(dev);
  for (auto& s : seen)
    for (bool& b : s) r.b(b);
  if (!r.ok) return false;
  spawn_.load(p);
  retryN_ = 0;
  swept_ = swept;
  subspace_ = sub;
  for (int l = 0; l < 3; l++)
    for (int k = 0; k < 8; k++) {
      holdDev_[l][k] = std::clamp(dev[l][k], -3, 3);
      holdSeen_[l][k] = seen[l][k];
    }
  if (r.p + SpawnCatchUp::kColsSize <= r.end) {
    spawn_.loadCols(r.p);
    r.p += SpawnCatchUp::kColsSize;
  } else {
    r.ok = false;
  }
  bool fade;
  int frames, bright, off[3][8], adj[3][8];
  uint8_t area;
  r.b(fade);
  r.v(frames);
  r.v(area);
  r.v(bright);
  r.v(off);
  r.v(adj);
  if (r.ok) {
    endFade_ = fade;
    endFrames_ = std::clamp(frames, 0, 1000);
    shownArea_ = area;
    lastBright_ = std::clamp(bright, 0, 15);
    for (int l = 0; l < 3; l++)
      for (int k = 0; k < 8; k++) {
        bandOff_[l][k] = std::clamp(off[l][k], -512, 512);
        bandAdj_[l][k] = std::clamp(adj[l][k], -1024, 1024);
      }
  } else {
    endFade_ = false;
    shownArea_ = ram(0x0534);
  }
  // (Older states kept a jar in subspace_.)
  bool jar = false;
  r.b(jar);
  jar_ = r.ok ? jar : sub && (ram(0x0628) & 0x0f) == 1;
  if (jar_) subspace_ = false;
  // (A state from before generators kept to the 4:3 edges may have one
  // stopped that the 4:3 game would have running: as the 4:3 game would
  // have it coming this way, the last one its right edge has met.)
  uint8_t gen43 = 0;
  r.v(gen43);
  if (!r.ok || !gen43) restoreGenerator();
  return true;
}

void Smb2Engine::restoreGenerator() {
  if (ram(0x00eb) == 0 || (ram(0x0628) & 0x0f) == 2) return;
  int edge = ((ram(0x04bf) << 8 | ram(0x04c1)) + 0x118) & ~15;
  int list = ram(0x00cd) << 8 | ram(0x00cc), y = 0, best = -1;
  uint8_t type = 0;
  for (int p = 0; p < 10 && list + y < 0x2000; p++) {
    int n = snes_.wram[list + y];
    if (n == 0) break;
    for (int k = 1; k + 1 < n && list + y + k + 1 < 0x2000; k += 2) {
      uint8_t t = snes_.wram[list + y + k];
      int x = p << 8 | (snes_.wram[list + y + k + 1] & 0xf0);
      if (t >= 0x47 && t < 0x5c && x <= edge && x > best) best = x, type = t;
    }
    y += n;
  }
  if (best >= 0) snes_.wram[0x04b9] = type;
}

// The wide view is centred on the game's screen, except at the ends of a
// horizontal area: there it stops at the area edge (instead of showing space
// outside it) and the unused width goes to the other side, so the player can
// walk right up to the display edge. The game's camera is never changed.
void Smb2Engine::viewSplit(int cam, int* l, int* r) const {
  int e = ext_, L = e, R = e;
  if (e > 0 && horizontal_ && !jar_ && !subspace_) {  // (a jar's room and subspace lie past the area's end: centred)
    int areaEnd = (ram(0x0541) + 1) * 256;
    if (areaEnd >= 256 + 2 * e) {
      if (cam - L < 0) {
        int s = L - std::max(cam, 0);
        L -= s;
        R += s;
      } else if (cam + 256 + R > areaEnd) {
        int s = std::min(cam + 256 + R - areaEnd, R);
        R -= s;
        L += s;
      }
    }
  }
  *l = L;
  *r = R;
}

void Smb2Engine::beginLine(Ppu& ppu, int row) {
  if (ext_ == 0) {
    if (row == 0 && trueX_) {
      ppu.clearObjTrueX();
      trueX_ = false;
    }
    return;
  }
  if (wallTile_[0] >= 0 || openVertical() || (roomJar_ && !jar_) || oneRoomH_)
    ppu.wideFetchAll[0] = true;  // (the walls' edge columns: see fetchWideTile, openVerticalTile)
  if (row == 0) bg1Vofs_ = ppu.bgVofs(0);  // (see openVerticalTile)
  // An area that wraps round (open vertical areas): the player can't go into
  // the margins, he comes back in at the 4:3 picture's other edge; dimmed,
  // they read as scenery beside the play space (an option). (Not the HUD:
  // its hearts are sprites, drawn after.)
  // (A one-screen room of a vertical area wraps round the same way unless
  // walled, then it is no room (see findWalledVertical): 4-3's Fryguy. Only
  // beside an edge the player can get to: not 5-2's jar-shaped room, nor its
  // climb's right, a wall all the way up.)
  if (dimWrap_ && row < 224 && (openVertical() || (roomJar_ && !jar_))) {
    ppu.dimLeft = dimAmt_[0];
    ppu.dimRight = dimAmt_[1];
  }
  // Beside a frame round the room (see runOpen_), the margins are outside
  // it: not in the windows either (6-1's jar-shaped room shades its inside
  // through the colour window, which met the right edge there and went on).
  if (openVertical() && row < 224) {
    int ar = ((ram(0x00ca) << 8 | ram(0x00cb)) + row) / 16;
    if (ar < 160 && (runOpen_[0][ar] || runOpen_[1][ar])) ppu.wideWindowMode = 1;
  }
  if (row == 0) {
    // Before the first line this frame's NMI has set the camera.
    cam16_ = horizontal_ ? (ram(0x04bf) << 8 | ram(0x04c1)) : 0;
    viewSplit(cam16_, &extL_, &extR_);
    ppu.setExt(extL_, extR_);
    prevShift_ = shift_;
    shift_ = ext_ - extL_;
    dark_ = ppu.forcedBlank() || ppu.brightness() == 0;
    lineHofs_[1] = lineHofs_[2] = -1;
    for (int l = 1; l <= 2; l++) {
      for (int b = 0; b < 8; b++) prevBandRow_[l][b] = b < bandIdx_[l] ? bandRow_[l][b] : -1;
    }
    bandIdx_[1] = bandIdx_[2] = 0;
    placeHudSprites(ppu);
    layerEdges();
    if (jar_ || roomJar_ || oneRoomH_ || openVertical() || wallTile_[0] >= 0) jarEdges(ppu);
  }
  // Some areas scroll bands of a layer at different speeds (night skies: the
  // sky band at 1/4, the hills below at 1/2): work out each band as it starts.
  // (A band also starts where one did last frame: two bands can share a
  // scroll value for a moment, 2-1's sky at 1/4 and dunes at 1/2 XOR $100
  // near camera 1020.)
  for (int l = 1; l <= 2; l++) {
    int h = ppu.bgHofs(l);
    int b = bandIdx_[l];
    if (h != lineHofs_[l] || (b > 0 && b < 8 && row == prevBandRow_[l][b])) {
      lineHofs_[l] = h;
      if (b < 8) bandRow_[l][b] = row;
      layerLine(l, h, bandIdx_[l]++);
    }
    ppu.bgHofsAdj[l] = bgAdj_[l];
    ppu.wideFetchAll[l] = bgAdj_[l] != 0;
  }
  // Paused: the undimmed cut-out of the dimmed screen (a window) goes with
  // the box (see placeHudSprites).
  if (gameplay_ && ram(0x022b) != 0) ppu.winShift = ext_ - extL_;
  // Leaving an area, the last (dim) frame of the fade already has the next
  // area's number ($0534): the margins, drawn from its data, go black.
  if (row == 0) {
    int b = ppu.forcedBlank() ? 0 : ppu.brightness();
    // (the area last shown fully lit or fading in; a changed one while
    // fading out isn't drawn yet)
    if (b == 15 || b > lastBright_) shownArea_ = ram(0x0534);
    maskArea_ = ram(0x0534) != shownArea_;
    lastBright_ = b;
  }
  if (maskArea_) ppu.maskWide = true;
}

// The sprites on screen now were placed last frame, for last frame's view:
// the life meter (OAM #0 on, pinned to the display edge) trailed the view by
// a frame while it moved at an area edge, and the pause box ($022B: its
// opening, 0 when not paused) stayed at its 4:3 spot. Place them for this
// frame's view: the meter at the edge, the box centred.
void Smb2Engine::placeHudSprites(Ppu& ppu) {
  int hearts = gameplay_ && hudAnchor_ ? std::min(ram(0x04c4) + 2, 4) : 0;
  bool paused = gameplay_ && ram(0x022b) != 0;
  for (int i = 0; i < 128; i++) {
    bool set = false;
    if (i < hearts) {
      ppu.objTrueX[i] = (int16_t)(8 - extL_);
      set = true;
    } else if (paused) {
      int x9 = ppu.oam[i * 4] | ((ppu.oam[512 + (i >> 2)] >> ((i & 3) * 2)) & 1) << 8;
      if (x9 < 256) {
        ppu.objTrueX[i] = (int16_t)(x9 + ext_ - extL_);
        set = true;
      }
    }
    ppu.objTrueXSet[i] = set;
  }
  trueX_ = true;
}

// A BG2/BG3 scroll value is (speed x camera) rounded either way, often XOR
// $100. Find which, for this band: its absolute scroll feeds the RAM renderer
// and, while the view is held at an area edge, its adjustment. The held view
// stands in for a camera `shift_` px away; each band keeps the rounding it had
// when the hold began, so it neither twitches while held nor steps on entry.
void Smb2Engine::layerLine(int l, int h, int band) {
  struct Kind {
    double speed;
    int x;
  };
  static const Kind kKinds[] = {{0.5, 0x100}, {0.25, 0x100}, {1.0, 0x100}, {0.25, 0},
                                {0.5, 0},     {1.0, 0},      {0.0, 0x100}, {0.0, 0}};
  constexpr int kCount = sizeof(kKinds) / sizeof(kKinds[0]);
  // (the layer's usual speed first; BG2 is the camera itself where $02C3 is
  // set, $14:EEC6, the boss rooms: at camera 0 every kind fits, and taken
  // for half speed it slid back into place as the player walked in)
  int first = l == 1 ? (ram(0x02c3) ? 2 : 0) : (ram(0x0772) ? 2 : 1);
  auto deviation = [&](int c) {
    int d = (h ^ kKinds[c].x) - (int)std::floor(kKinds[c].speed * cam16_);
    return ((d % 512) + 512 + 256) % 512 - 256;
  };
  // A band keeps the kind it had last frame while it still fits: its scroll
  // can drift from speed x camera by more than the kinds can be told apart by
  // (the dunes under 2-1's night sky run up to 2 px ahead).
  int bi = std::min(band, 7);
  int8_t& known = bandKind_[l][bi];
  int8_t before = known;
  int k = -1, dev = 0;
  if (known >= 0 && std::abs(deviation(known)) <= 3) {
    k = known;
    dev = deviation(k);
  }
  for (int i = 0; i < kCount && k < 0; i++) {
    int c = i == 0 ? first : (i == first ? 0 : i);
    int d = deviation(c);
    if (std::abs(d) <= 1) {
      k = c;
      dev = d;
    }
  }
  known = (int8_t)k;
  bgAdj_[l] = 0;
  if (k < 0) {  // not a camera-relative scroll: leave it alone
    layerAbs_[l] = layerScrollX(l);
    return;
  }
  double sp = kKinds[k].speed;
  int base = (int)std::floor(sp * cam16_);
  layerAbs_[l] = base + dev;
  int& d0 = holdDev_[l][k];
  if (!shift_ || !holdSeen_[l][k]) {
    d0 = dev;  // (latched while held)
    holdSeen_[l][k] = true;
  }
  if (shift_ && sp != 1.0) {
    int want = (int)std::floor(sp * (cam16_ + shift_)) + d0;
    bgAdj_[l] = (int16_t)(want - shift_ - layerAbs_[l]);
  }
  // A band whose kind turns out different (the boss rooms' 1:1 background
  // fits every kind at camera 0) stays where it is: the difference goes
  // while the screen is dark, or 1 px a frame while the view shifts.
  int& off = bandOff_[l][bi];
  if (dark_) off = 0;
  else if (before >= 0 && k != before) off = bandAdj_[l][bi] - bgAdj_[l];
  else if (off != 0 && shift_ != prevShift_) off -= off > 0 ? 1 : -1;
  bgAdj_[l] = (int16_t)(bgAdj_[l] + off);
  bandAdj_[l][bi] = bgAdj_[l];
}

// ---------------------------------------------------------------------------
// RAM renderers (see docs/re/smb2.md §4.4)

// A vertical area walled both sides all the way up (1-3's tower: its edge
// columns one tile each in every row, not the area's background): no way
// round, so nothing wraps in view; shown wide, the walls carried on. Their
// inside: the commonest tile next to each edge column that is neither the
// edge nor the background (1-3's bricks), else the edge tile itself.
// (Also what is beside what in it, for openVerticalTile: open vertical areas
// and one-screen horizontal ones.)
void Smb2Engine::findWalledVertical() {
  wallTile_[0] = wallTile_[1] = -1;
  if ((!verticalArea_ && !oneRoomH_) || jar_ || subspace_) return;
  const uint8_t* w = snes_.wram;
  int base = 0x2000 + (ram(0x0534) & 0x0f) * 0xb00, pages = ram(0x0541) + 1;
  if (pages > 10) return;
  std::map<int, int> all;
  for (int i = 0; i < pages * 0xf0; i++) all[w[base + i]]++;
  int bg = -1, most = 0;
  for (auto& kv : all)
    if (kv.second > most) bg = kv.first, most = kv.second;
  openBg_ = bg;
  // (which tile is most often beside which, each way, for openVerticalTile)
  {
    // (keyed by tile + $100 for the alternate-graphics flag: 2-1's sandstone)
    static std::map<int, int> left[512], right[512], outL[512], outR[512];
    for (auto* g : {left, right, outL, outR})
      for (int t = 0; t < 512; t++) g[t].clear();
    auto key = [&](int o) { return w[o] | (w[o + 0x7000] ? 0x100 : 0); };
    for (bool& b : selfH_) b = false;
    // (how often each tile has something beside it towards the nearer edge,
    // out of how often it is seen there: see noneL_)
    static int nearL[512], nearR[512], solidL[512], solidR[512];
    for (int t = 0; t < 512; t++) nearL[t] = nearR[t] = solidL[t] = solidR[t] = 0;
    for (int r = 0; r < pages * 15; r++)
      for (int c = 1; c < 16; c++) {
        int a = key(base + r * 16 + c - 1), b = key(base + r * 16 + c);
        if (c <= 8) nearL[b]++, solidL[b] += !passable(a);
        if (c - 1 >= 7) nearR[a]++, solidR[a] += !passable(b);
        if (a == 0x40 || b == 0x40) continue;
        if (a == b) selfH_[a] = true;  // (a tile beside itself: one wall of one tile)
        if (a == b) continue;
        left[b][a]++;
        right[a][b]++;
        // (beyond it towards the nearer edge only: a room's frame, the same
        // tile both sides, has the room inside it on the other side (6-1's
        // jar-shaped room, its frame at the screen's edges))
        if (c <= 8) outL[b][a]++;
        if (c - 1 >= 7) outR[a][b]++;
      }
    // (what tops each tile where it is a surface: the tile above it with
    // blank above that, most often; see openVerticalTile)
    {
      static std::map<int, int> surf[512];
      for (auto& m : surf) m.clear();
      for (int r = 1; r < pages * 15; r++)
        for (int c = 0; c < 16; c++) {
          int t = key(base + (r / 15) * 0xf0 + (r % 15) * 16 + c);
          int u = key(base + ((r - 1) / 15) * 0xf0 + ((r - 1) % 15) * 16 + c);
          int v = r >= 2 ? key(base + ((r - 2) / 15) * 0xf0 + ((r - 2) % 15) * 16 + c) : 0x40;
          if (t != 0x40 && u != 0x40 && v == 0x40 && !passable(u)) surf[t][u]++;
        }
      for (int t = 0; t < 512; t++) {
        int best = -1, n = 1;
        for (auto& kv : surf[t])
          if (kv.second > n) best = kv.first, n = kv.second;
        surfaceOf_[t] = (int16_t)best;
      }
    }
    // (only what is seen beside it often: a pair seen once or twice led into
    // odd tiles, 5-3's logs and pillars out in the sand)
    // (and which tiles are the walls': those seen in the edge columns)
    for (bool& b : edgeTile_) b = false;
    for (int r = 0; r < pages * 15; r++) {
      edgeTile_[key(base + r * 16)] = true;
      edgeTile_[key(base + r * 16 + 15)] = true;
    }
    for (int t = 0; t < 512; t++) edgeOnly_[t] = edgeTile_[t];
    for (int r = 0; r < pages * 15; r++)
      for (int c = 1; c < 15; c++) edgeOnly_[key(base + r * 16 + c)] = false;
    // (A wall's inner tiles: with wall tiles both sides of them most often.
    // Its faces (wall one side, sand or air the other) are left out of what
    // carries on: 2-3's pyramid, the face beside the most common inner tile
    // more often than its partner, came out every other block, banding.)
    auto top = [&](const std::map<int, int>& m) {
      int best = -1, n = 0;
      for (auto& kv : m)
        if (kv.second > n) best = kv.first, n = kv.second;
      return best;
    };
    bool* inner = innerTile_;
    openFill_ = -1;
    int fillN = 0;
    for (int t = 0; t < 512; t++) {
      int l = top(left[t]), r = top(right[t]);
      // (not the area's commonest tile: the open space it's built in, 3-1's
      // waterfall a tile of this layer, carried on as hills' insides)
      // (Open space blank ($40, not counted): beside walls' tiles both ways.
      // Open space a tile of this layer (3-1's waterfall, the commonest): beside
      // no open space either way and walls on one side at least; its ground's
      // pieces are inside a face that is not at the screen's edge.)
      bool open = bg != 0x40;
      inner[t] = l >= 0 && r >= 0 && t != bg && l != bg && r != bg &&
                 (open ? edgeTile_[l] || edgeTile_[r] : edgeTile_[l] && edgeTile_[r]);
      int n = 0;
      for (auto& kv : left[t]) n += kv.second;
      if (inner[t] && edgeTile_[t] && n > fillN) openFill_ = t, fillN = n;  // (the walls' commonest inner tile)
      // (and what is most often beside it each way, any tile)
      rawL_[t] = (int16_t)top(left[t]);
      rawR_[t] = (int16_t)top(right[t]);
      // (that, seen twice at least, else itself if it is beside itself;
      // -1: nothing (a pair seen once: 5-2's vine beside a cloud's end))
      auto seen2 = [&](const std::map<int, int>& m) {
        int best = top(m);
        auto it = best >= 0 ? m.find(best) : m.end();
        return it != m.end() && it->second >= 2 ? best : selfH_[t] ? t : -1;
      };
      beyondL_[t] = (int16_t)seen2(outL[t]);
      beyondR_[t] = (int16_t)seen2(outR[t]);
      // (an end piece: something beside it that way in a quarter of the
      // places it is seen at most (5-2's and 6-3's cloud platforms' ends, a
      // vine behind one now and then); a corner seen beside a tile or two
      // is a wall's (6-3's vine shaft))
      noneL_[t] = nearL[t] > 0 && solidL[t] * 4 <= nearL[t];
      noneR_[t] = nearR[t] > 0 && solidR[t] * 4 <= nearR[t];
    }
    // (Each run of the edge column, top to bottom: open beyond it if a tile
    // in it has nothing beyond it ever (a frame round the room), the corner
    // pieces drawn only at the edge with it (6-1's jar-shaped room).)
    for (int side = 0; side < 2; side++) {
      int rows = std::min(pages * 15, 160);
      for (int r = 0; r < rows;) {
        int t = key(base + (r / 15) * 0xf0 + (r % 15) * 16 + (side ? 15 : 0));
        if (t == 0x40) {
          runOpen_[side][r++] = false;
          continue;
        }
        int r1 = r;
        bool open = false, wall = false;
        auto edgeKey = [&](int q) { return key(base + (q / 15) * 0xf0 + (q % 15) * 16 + (side ? 15 : 0)); };
        // (an end piece: nothing beyond it in a quarter of its places at
        // most, see noneL_; a wall's piece: something beyond it, known)
        auto ends = [&](int u) { return !edgeOnly_[u] && (side ? noneR_ : noneL_)[u]; };
        auto walls = [&](int u) { return !edgeOnly_[u] && !(side ? noneR_ : noneL_)[u]; };
        for (; r1 < rows; r1++) {
          int u = edgeKey(r1);
          if (u == 0x40) break;
          open |= ends(u);
          wall |= walls(u);
        }
        // (Only end pieces and pieces drawn only for the edge: a frame,
        // open all the way. A wall's pieces in it too (7-1's Birdo room, its
        // brick wall on the cloud floor's end): row by row, the wall going
        // on, and the end piece under it as what it ends (the floor under
        // the wall carried on).)
        for (int q = r; q < r1; q++) {
          bool o = open, under = wall && ends(edgeKey(q)) && q > r && walls(edgeKey(q - 1));
          if (wall) o = ends(edgeKey(q)) && !under;
          runOpen_[side][q] = o;
          runEnd_[side][q] = r1 - 1;
          floorUnder_[side][q] = under;
        }
        r = r1;
      }
    }
    for (int t = 0; t < 512; t++) {
      nextL_[t] = nextR_[t] = -1;
      int bl = 2, br = 2;
      for (auto& kv : left[t])
        if (kv.second > bl && inner[kv.first]) nextL_[t] = (int16_t)kv.first, bl = kv.second;
      for (auto& kv : right[t])
        if (kv.second > br && inner[kv.first]) nextR_[t] = (int16_t)kv.first, br = kv.second;
    }
  }
  if (oneRoomH_) return;
  int edge[2] = {w[base], w[base + 15]}, inner[2] = {-1, -1};
  for (int side = 0; side < 2; side++) {
    int c = side ? 15 : 0, k = side ? 14 : 1;
    if (edge[side] == bg) return;
    std::map<int, int> next;
    for (int r = 0; r < pages * 15; r++) {
      if (w[base + r * 16 + c] != edge[side]) return;
      int t = w[base + r * 16 + k];
      if (t != bg && t != edge[side] && !w[base + r * 16 + k + 0x7000]) next[t]++;
    }
    int best = 0;
    for (auto& kv : next)
      if (kv.second > best) inner[side] = kv.first, best = kv.second;
  }
  for (int side = 0; side < 2; side++) wallTile_[side] = inner[side] >= 0 ? inner[side] : edge[side];
  // (A room one screen tall walled so (1-3's key room, the boss rooms): its
  // walls carried on, not a jar's black round it.)
  roomJar_ = false;
}

// Which of the screen's edges, where a vertical area wraps round, the player
// can get to (1 the left, 2 the right): through open space (blank, or the
// area's own open-space tile) from where he is, everything else taken as
// solid. -1: unsure.
// Tiles the player goes through: blank, and what he climbs (the list at
// $11:9B50, checked at $12:819D: vines, ladders, chains), not its entries
// that are other tilesets' walls ($07, $18, $19, $27: 6-3's dirt). A vine
// beside a cloud platform's end, or in a shaft, is no wall (6-3).
bool Smb2Engine::passable(int key) {
  switch (key) {
    case 0x40: case 0xc1: case 0xc2: case 0xc3: case 0xc4: case 0xd4:
    case 0x80: case 0x81: case 0x9e: case 0x9f:
      return true;
  }
  return false;
}

int Smb2Engine::playerReachesEdge() const {
  if (horizontal_) return 3;
  const uint8_t* w = snes_.wram;
  int base = 0x2000 + (ram(0x0534) & 0x0f) * 0xb00, pages = ram(0x0541) + 1;
  if (pages > 10) return -1;
  int rows = pages * 15;
  auto open = [&](int r, int c) {
    int o = base + (r / 15) * 0xf0 + (r % 15) * 16 + c;
    return (w[o] == openBg_ && !w[o + 0x7000]) || passable(w[o] | (w[o + 0x7000] ? 0x100 : 0));
  };
  // (the rows on screen, and one beyond each way)
  int top = (ram(0x00ca) << 8 | ram(0x00cb)) / 16 - 1, bottom = top + 16;
  int c0 = ram(0x0028) >> 4, r0 = ram(0x001e) * 15 + std::min(ram(0x0032), (uint8_t)0xef) / 16;
  if (r0 >= rows) return -1;
  // (from where he is, open or not: on a vine, a ladder)
  std::vector<uint8_t> seen(rows * 16, 0);
  int sides = 0;
  std::vector<int> todo{r0 * 16 + c0};
  seen[r0 * 16 + c0] = 1;
  while (!todo.empty()) {
    int r = todo.back() / 16, c = todo.back() % 16;
    todo.pop_back();
    // (an edge reached in open space only: come round into the wall at the
    // other edge (5-2's climb, off its left), he is not in it)
    if (open(r, c)) sides |= c == 0 ? 1 : c == 15 ? 2 : 0;
    const int dr[] = {1, -1, 0, 0}, dc[] = {0, 0, 1, -1};
    for (int k = 0; k < 4; k++) {
      int rr = r + dr[k], cc = c + dc[k];
      // (cc must be checked too: at a side edge it is -1 or 16, which indexed `seen` a row off, and
      // before its first byte at row 0 / past its last at the bottom row. That wrote into the heap
      // around the buffer: a crash when it was freed. An edge column is where the fill stops.)
      if (rr < 0 || rr >= rows || cc < 0 || cc > 15 || rr < top || rr > bottom || seen[rr * 16 + cc] ||
          !open(rr, cc))
        continue;
      seen[rr * 16 + cc] = 1;
      todo.push_back(rr * 16 + cc);
    }
  }
  return sides;
}

// An open vertical area (not walled both sides all the way up) is wide too
// (user, 2026-10-29). It wraps round: the player and enemies walking off one
// side come back on the other, at the 4:3 picture's edges, as the game does.
bool Smb2Engine::openVertical() const {
  return verticalArea_ && !jar_ && !subspace_ && !roomJar_ && wallTile_[0] < 0;
}

// An open vertical area's margins (BG1), per row: what reaches the screen's
// edge (the edge tile and the one inside it, neither blank $40) carries on,
// each tile further out the one most often beside the last on that side in
// the area (2-1's pyramid: its sandstone wall, rows of pieces two to a block,
// carried on in step; another tile inside the edge put rows of bricks out of
// line). An edge tile seen nowhere else (1-3's faces, 2-2's cave): the tile
// inside it carried on, or the other edge's if that is a face (2-2's left
// wall, a face carried on came out in stripes). Elsewhere nothing, the layers
// behind showing. (An area never blank at its edges is walled all the way.)
int Smb2Engine::openVerticalTile(int tx, int ty, int screenX, uint16_t* entry, bool oneScreen, int* keyOut) const {
  // What the answer depends on besides the tile: WRAM and the line's state (the same while one line is drawn: the
  // emulated clock stands still then), BG1's scroll, the margins, and of screenX only its 16 px column (the edge
  // tests at 0, 16, 128, 240 and 256, the columns out from the edge) and whether it is in the margin's last 16 px.
  if (keyOut) return openVerticalTileRaw(tx, ty, screenX, entry, oneScreen, keyOut);
  const uint64_t stamp = snes_.masterCycles();
  const int col = screenX >> 4;
  const bool last = screenX < 0 ? screenX < -extL_ + 16 : screenX >= 256 + extR_ - 16;
  OvtMemo& m = ovtMemo_;
  if (m.stamp == stamp && m.tx == tx && m.ty == ty && m.col == col && m.last == last && m.oneScreen == oneScreen &&
      m.vofs == bg1Vofs_ && m.extL == extL_ && m.extR == extR_) {
    if (m.r == PpuHooks::kWideEntry) *entry = m.e;
    return m.r;
  }
  uint16_t e = 0;
  const int r = openVerticalTileRaw(tx, ty, screenX, &e, oneScreen, nullptr);
  if (r == PpuHooks::kWideEntry) *entry = e;
  m.stamp = stamp, m.tx = tx, m.ty = ty, m.col = col, m.last = last, m.oneScreen = oneScreen;
  m.vofs = bg1Vofs_, m.extL = extL_, m.extR = extR_, m.r = r, m.e = e;
  return r;
}

int Smb2Engine::openVerticalTileRaw(int tx, int ty, int screenX, uint16_t* entry, bool oneScreen, int* keyOut) const {
  const uint8_t* w = snes_.wram;
  int base = 0x2000 + (ram(0x0534) & 0x0f) * 0xb00, pages = ram(0x0541) + 1;
  int camY = ram(0x00ca) << 8 | ram(0x00cb);
  // (BG1's map row to the area's: BG1VOFS is the camera Y plus an offset, a
  // multiple of 16, that is $100 only near the area's top (2-3's lower
  // shaft: camera $2D0, BG1VOFS $230), taken from the scroll as shown;
  // the scroll trails the camera by a few px while it moves)
  int k = ((bg1Vofs_ - camY + 8) & ~15) & 511;
  int y = ((ty & 63) * 8 - k) & 511;
  y += ((camY + 112 - y + 256) / 512 - (camY + 112 - y + 256 < 0 ? 1 : 0)) * 512;
  if (oneScreen) y = (ty & 63) * 8;  // (a horizontal area's: BG1VOFS 0)
  if (y < 0 || y >= pages * 240) return PpuHooks::kWideBlank;
  int page = y / 240, row = (y % 240) >> 4;
  auto at = [&](int c) { return base + page * 0xf0 + row * 16 + c; };
  // The edge column itself (the game's 256 px, see beginLine): a face drawn
  // for the screen's edge, seen nowhere else (8-1's bricks' outlined end),
  // stood out as a seam beside the wall carried on: what the margin shows
  // next to it instead.
  // (The floor's end under a wall carried on (see findWalledVertical):
  // carried on under it too, its end piece at the margin's far side; in the
  // game's own edge column a middle piece, not a join.)
  int fside = screenX < 128 ? 0 : 1, frow = page * 15 + row;
  if (frow < 160 && floorUnder_[fside][frow]) {
    int ed = at(fside ? 15 : 0), in1 = at(fside ? 14 : 1);
    int ek = w[ed] | (w[ed + 0x7000] ? 0x100 : 0), ik = w[in1] | (w[in1 + 0x7000] ? 0x100 : 0);
    int far = fside ? 256 + extR_ - 16 : -extL_;  // (the margin's last 16 px)
    bool last = fside ? screenX >= far : screenX < far + 16;
    if (ik != 0x40) {
      *entry = keyEntry(last && (screenX < 0 || screenX >= 256) ? ek : ik, tx & 1, ty & 1);
      if (keyOut) *keyOut = ek;
      return PpuHooks::kWideEntry;
    }
  }
  if (screenX >= 0 && screenX < 256) {
    // (only where the margin goes on with what that face has inside it,
    // 8-1's bricks: not blocks of their own at the edge, 4-3's ice)
    int ed = at(screenX < 128 ? 0 : 15), k = w[ed] | (w[ed + 0x7000] ? 0x100 : 0);
    if (!edgeOnly_[k] || innerTile_[k]) return PpuHooks::kWideVram;
    int inside = (screenX < 128 ? rawR_ : rawL_)[k];
    if (inside < 0 || inside == openBg_) return PpuHooks::kWideVram;
    int r = openVerticalTile(tx, ty, screenX < 128 ? -1 : 256, entry, oneScreen);
    if (r != PpuHooks::kWideEntry || *entry != keyEntry(inside, tx & 1, ty & 1)) return PpuHooks::kWideVram;
    return r;
  }
  int side = screenX < 0 ? 0 : 1, e = at(side ? 15 : 0), in = at(side ? 14 : 1), in2 = at(side ? 13 : 2);
  constexpr int kBlank = 0x40;
  // (Blank inside the edge: carried on only as a wall, the edge column solid
  // above and below too (2-1's pyramid's foot, its wall's face beside the
  // room); a block alone at the edge, a platform's end, is not.)
  auto edgeAt = [&](int dy) {
    int yy = y + dy * 16;
    if (yy < 0 || yy >= pages * 240) return true;
    return w[base + (yy / 240) * 0xf0 + ((yy % 240) >> 4) * 16 + (side ? 15 : 0)] != kBlank;
  };
  // (its top or bottom row too: solid one way, 8-1's brick wall, its top
  // row with the room beside it left out)
  if (w[e] == kBlank || (w[in] == kBlank && !edgeAt(-1) && !edgeAt(1))) return PpuHooks::kWideBlank;
  // (open space that is a tile of this layer, 3-1's waterfall: it goes on)
  if (w[e] == openBg_ && !w[e + 0x7000]) {
    *entry = keyEntry(openBg_, tx & 1, ty & 1);
    if (keyOut) *keyOut = openBg_;
    return PpuHooks::kWideEntry;
  }
  auto key = [&](int o) { return w[o] | (w[o + 0x7000] ? 0x100 : 0); };
  int mid = -1;
  const int16_t* next = side ? nextR_ : nextL_;
  int d = side ? (screenX - 256) / 16 + 1 : (15 - screenX) / 16;  // (columns out from the edge)
  // (What is beyond a tile on this side in the area (see findWalledVertical))
  auto out = [&](int t) { return (side ? beyondR_ : beyondL_)[t]; };
  // (An end piece, nothing ever beyond it this way (5-2's cloud platforms'
  // rounded ends, one at the screen's edge), or a frame of them round a
  // room (see findWalledVertical): the margin open. Not a face drawn only
  // for the edge on its own: nothing is ever beyond those either.)
  if (page * 15 + row < 160 && runOpen_[side][page * 15 + row]) return PpuHooks::kWideBlank;
  // (The row's own tiles inside the edge repeating every one or two: that
  // goes on, in step: 2-3's pyramid's walls, 06 05 06 05.)
  int t0 = key(e), t1 = key(in), t2 = key(in2), t3 = key(at(side ? 12 : 3));
  if (t1 != kBlank && t2 != kBlank && t0 == t2 && (t1 == t3 || t3 == kBlank) && innerTile_[t0] && innerTile_[t1]) {
    *entry = keyEntry(d % 2 ? t1 : t0, tx & 1, ty & 1);
    if (keyOut) *keyOut = d % 2 ? t1 : t0;
    return PpuHooks::kWideEntry;
  }
  int o1 = at(side ? 1 : 14), o2 = at(side ? 2 : 13);
  bool across = w[in2] != w[in] && w[o1] != kBlank && w[o1] != openBg_ && w[o1] == w[o2];
  if (next[key(e)] >= 0) {
    // (Through the walls' tiles only: past them, the last two turn about,
    // a wall of two pieces in step; 5-3's logs and pillars beside its sand
    // walls, a sand shaft in 2-3's, stayed out.)
    int prev = -1;
    mid = key(e);
    for (int k = 0; k < d; k++) {
      int n = next[mid];
      if (n < 0 || !edgeTile_[n]) n = prev >= 0 ? prev : mid;
      prev = mid;
      mid = n;
    }
  } else {
    if (across) in = o1;  // (the row goes on from the other edge)
    // (a wall with nothing inside it: what that face has inside it elsewhere
    // (8-1's bricks beside their end piece), else the wall itself, 2-3's sand)
    // (and a face drawn only at the screen's edge (4-3's tower: its walls'
    // outlined pieces): the wall goes on, what the room has against it
    // inside, spikes or a ledge of ice, stays inside)
    int inside = (side ? rawL_ : rawR_)[key(e)];
    bool wallIn = inside >= 0 && inside != openBg_ && inside != kBlank;
    if (w[in] == kBlank) mid = wallIn ? inside : key(e);
    else if (!across && edgeOnly_[key(e)] && !innerTile_[key(e)] && wallIn) mid = inside;
    else mid = key(in);
    // (a face, which repeated came out in stripes: the walls' commonest
    // inner tile, 4-3's tower's bricks beside its column pieces)
    if (!innerTile_[mid] && openFill_ >= 0 && (w[in] == kBlank || key(in2) != mid)) {
      mid = openFill_;  // (and on out from it as the walls go: 06 05 06, not 06 06 06)
      for (int k = 1, prev = -1; k < d; k++) {
        int n = next[mid] >= 0 && innerTile_[next[mid]] ? next[mid] : (prev >= 0 ? prev : mid);
        prev = mid;
        mid = n;
      }
    }
  }
  // (A wall's face carried on, the edge tile or the one inside it, never
  // beside itself in the area, which repeated came out in stripes (5-2's
  // climb, its dirt wall's outlined face): what is beyond the edge tile
  // where the wall is thicker instead, on out through the walls' tiles
  // (04 05 06 05).)
  auto wallish = [&](int t) { return t >= 0 && t != kBlank && t != openBg_ && (edgeTile_[t] || innerTile_[t]); };
  int beyond = out(key(e));
  if (!innerTile_[mid] && !selfH_[mid] && (mid == key(e) || mid == key(in)) && !across &&
      !edgeOnly_[key(e)] && beyond != key(e) && wallish(beyond) && edgeTile_[beyond]) {
    mid = key(e);
    for (int k = 0, prev = -1; k < d; k++) {
      int n = out(mid);
      if (!wallish(n)) n = prev >= 0 ? prev : mid;
      prev = mid;
      mid = n;
    }
  }
  // (A wall's top row, its corner at the edge (6-3's vine shaft, its grassy
  // rounded corner): what tops the wall where it goes on below, the
  // margin's tile on the row under it, not the corner over and over.)
  // (or the row under that: the dirt's rows alternate, grass on one only)
  // (Not a row going on from the other edge: 3-3's ledge, on the bricks.)
  if (!edgeAt(-1) && !across) {
    for (int k = 1; k <= 2; k++) {
      int below = -1;
      uint16_t e2;
      if (openVerticalTile(tx, ty + 2 * k, screenX, &e2, oneScreen, &below) == PpuHooks::kWideEntry && below >= 0 &&
          surfaceOf_[below] >= 0) {
        mid = surfaceOf_[below];
        break;
      }
    }
  }
  *entry = keyEntry(mid, tx & 1, ty & 1);
  if (keyOut) *keyOut = mid;
  return PpuHooks::kWideEntry;
}

// A tile (+ $100: flagged, the alternate graphics) as the renderer draws it
// (see bg1Entry), its quarter qx, qy.
uint16_t Smb2Engine::keyEntry(int key, int qx, int qy) const {
  if (!(key & 0x100)) return metaEntry(key, qx, qy);
  uint16_t e = snes_.peek16((0x150000 | snes_.peek16(0x15a4c8)) + (key & 0xff) * 8 + qy * 4 + qx * 2);
  if (ram(0x0635) == 3) return (e & 0xff) ^ 0x0a00;
  if (ram(0x0773) & 0x0f) return (e & 0x1ff) | 0x1800;
  return e;
}

// A metatile's tilemap word (unflagged), its quarter qx, qy.
uint16_t Smb2Engine::metaEntry(int mid, int qx, int qy) const {
  int t = mid >> 6;
  uint32_t tab = 0x110000 | snes_.peek(0x11ad64 + t) | (snes_.peek(0x11ad68 + t) << 8);
  return snes_.peek16(tab + (mid & 0x3f) * 8 + qy * 4 + qx * 2);
}

bool Smb2Engine::bg1Entry(int x, int y, uint16_t* out) const {
  int pages = ram(0x0541) + 1;
  if (x < 0 || x >= pages * 256 || y < 0 || y >= 240) return false;
  const uint8_t* w = snes_.wram;
  int base = 0x2000 + (ram(0x0534) & 0x0f) * 0xb00;
  int p = x >> 8, c = (x >> 4) & 15, row = y >> 4;
  int world = ram(0x0635);
  int f628 = ram(0x0628) & 0x0f, f773 = ram(0x0773) & 0x0f;
  // A flagged (alternate) tile uses the alternate table plus the world-4 /
  // $0773 rules; the game resets those flags for every tile ($14F285). The
  // jar test uses $0720, the last unflagged tile id drawn above in the column.
  int last720 = 0;
  if (base + p * 0xf0 + row * 16 + c + 0x7000 >= 0x20000) return false;  // stay inside WRAM
  for (int r = 0; r < row; r++) {
    int off = base + p * 0xf0 + r * 16 + c;
    if (!w[off + 0x7000]) last720 = w[off];
  }
  int off = base + p * 0xf0 + row * 16 + c;
  int mid = w[off];
  // (In subspace a hidden mushroom ($41/$42) is one only on its own screen,
  // where the game made it ($12:F427): beside it, nothing.)
  if (f628 == 2 && (mid == 0x41 || mid == 0x42)) mid = 0x40;
  bool flagged = w[off + 0x7000] != 0;
  uint32_t tab;
  int idx;
  if (flagged) {
    tab = 0x150000 | snes_.peek16(0x15a4c8);
    idx = mid;
  } else {
    last720 = mid;
    int t = mid >> 6;
    tab = 0x110000 | snes_.peek(0x11ad64 + t) | (snes_.peek(0x11ad68 + t) << 8);
    idx = mid & 0x3f;
  }
  bool f769 = flagged && world == 3, f76c = flagged;
  uint16_t e = snes_.peek16(tab + idx * 8 + ((y >> 3) & 1) * 4 + ((x >> 3) & 1) * 2);
  if (f628 == 1 && last720 == 0xab) e |= 0x2000;
  else if (f769) e = (e & 0xff) ^ 0x0a00;
  else if (f773 && f76c) e = (e & 0x1ff) | 0x1800;
  *out = e;
  return true;
}

// A parallax layer's RAM map starts at x 0 and the game never shows what is
// left of that, but a wide view near an area start does (the layer moves
// slower than the playfield). What goes there:
// * BG3 outdoors is a loop of 4 pages ($14F538: 4 cloud patterns, in one of 8
//   orders picked by the frame counter as the area is entered; a cloud drawn
//   on the last page runs on into the first). Left of x 0 is its last page.
//   Climbing into 1-1's sky gave an order whose first page starts inside
//   that cloud: repeating the first page showed it cut off straight.
// * BG2 (decoded per area at level start) often repeats every 1 to 4 pages
//   (the last drawn page may stop short): the same loop is used.
// * Otherwise the first page repeats where its ends join up, and is mirrored
//   at x 0 where they don't (a hill or cloud would show cut off).
int Smb2Engine::layerBase(int layer) const {
  int base = layer == 1 ? 0x12000 + (ram(0x0534) & 0x0f) * 0x1000 : 0x1c000;
  return base + 16 * 0xf0 <= 0x20000 ? base : -1;  // (inside WRAM)
}

int Smb2Engine::layerPeriod(int layer, int* pages) const {
  *pages = 0;
  int base = layerBase(layer);
  if (base < 0) return 0;
  const uint8_t* w = snes_.wram + base;
  auto drawn = [&](int p) {
    for (int i = 0; i < 0xf0; i++)
      if (w[p * 0xf0 + i] != 0xff) return true;
    return false;
  };
  int last = -1;
  for (int p = 0; p < 16; p++)
    if (drawn(p)) last = p;
  *pages = last + 1;
  // compare whole pages only: the last drawn one may stop short
  for (int q = 1; q <= 4; q++) {
    if (last - q < 1) break;
    bool same = true;
    for (int k = 0; k + q < last && same; k++) same = std::memcmp(w + k * 0xf0, w + (k + q) * 0xf0, 0xf0) == 0;
    if (same) return q;
  }
  // (Two pages only (5-1's jungle, both the same): nothing to compare but the
  // last, whole; past them a held view at the area's right end showed none.)
  if (last == 1 && std::memcmp(w, w + 0xf0, 0xf0) == 0) return 1;
  return 0;
}

// Repeating the first page puts its last column against its first: a seam
// wherever one of the two is drawn and the other is empty.
bool Smb2Engine::startCut(int layer) const {
  int base = layerBase(layer);
  if (base < 0) return false;
  const uint8_t* w = snes_.wram + base;
  for (int r = 0; r < 15; r++)
    if ((w[r * 16] == 0xff) != (w[r * 16 + 15] == 0xff)) return true;
  return false;
}

void Smb2Engine::layerEdges() {
  for (int l = 1; l <= 2; l++) {
    layerLoop_[l] = layerEnd_[l] = 0;
    mirrorStart_[l] = false;
    if (!horizontal_) continue;
    if (l == 2 && !ram(0x0772)) {
      layerLoop_[l] = layerEnd_[l] = 4 * 256;  // (caves: BG3 is an overlay at the playfield's speed)
    } else if (l == 1) {
      int pages;
      layerLoop_[l] = layerPeriod(l, &pages) * 256;
      layerEnd_[l] = pages * 256;
    }
    if (!layerLoop_[l]) mirrorStart_[l] = startCut(l);
  }
}

bool Smb2Engine::bgLayerEntry(int layer, int x, int y, uint16_t* out) const {
  // x is a tile's left edge; mirrored, the tile [x, x+8) is [-8-x, -x) flipped.
  bool flip = false;
  if (layerLoop_[layer] && (x < 0 || x >= layerEnd_[layer])) {
    x %= layerLoop_[layer];
    if (x < 0) x += layerLoop_[layer];
  } else if (x < 0 && mirrorStart_[layer]) {
    x = -8 - x;
    flip = true;
  }
  while (x < 0) x += 256;
  if (y < 0 || y >= 240) return false;
  const uint8_t* w = snes_.wram;
  int base;
  uint32_t tab, empty;
  if (layer == 1) {
    base = 0x12000 + (ram(0x0534) & 0x0f) * 0x1000;
    tab = w[0x2df] | (w[0x2e0] << 8) | (w[0x2e1] << 16);
    empty = 0x130000 | snes_.peek16(0x13a613);
  } else {
    base = 0x1c000;
    tab = w[0x744] | (w[0x745] << 8) | (w[0x746] << 16);
    empty = 0x150000 | snes_.peek16(0x15934f);
  }
  int p = x >> 8;
  if (p >= 16 || !tab) return false;
  int moff = base + p * 0xf0 + (y >> 4) * 16 + ((x >> 4) & 15);
  if (moff >= 0x20000) return false;  // stay inside WRAM
  int mid = w[moff];
  uint32_t src = mid == 0xff ? empty : tab + mid * 8;
  *out = snes_.peek16(src + ((y >> 3) & 1) * 4 + ((x >> 3) & 1) * 2);
  if (flip) *out ^= 0x4000;
  return true;
}

int Smb2Engine::layerScrollX(int layer) const {
  if (layer == 0) return cam16_;
  if (layer == 1) return cam16_ >> 1;
  return ram(0x0772) ? cam16_ : cam16_ >> 2;
}

// A jar's scenery layer joins up at the screen's edges unless rows show
// something on one side and nothing (the layer's commonest tile) on the other.
// jarEdges works only from the inputs kept in JarInputs (and leaves openMirror_, openBlank_ and openKeep_ as they
// were when the area is neither open nor walled), so when they are all as last time its results stand as they are:
// most frames (the map and scroll still) it has nothing new to look at.
void Smb2Engine::jarEdges(const Ppu& ppu) {
  if (!jarIn_) jarIn_ = std::make_unique<JarInputs>();
  JarInputs& in = *jarIn_;
  bool same = jarInValid_;
  const bool open = openVertical() || wallTile_[0] >= 0;
  if (in.open != open) same = false;
  in.open = open;
  for (int i = 0; i < 2; i++) {
    const int l = i + 1;
    const uint16_t h = ppu.bgHofs(l), v = ppu.bgVofs(l), mb = ppu.bgMapBase(l);
    const uint8_t ms = ppu.bgMapSize(l);
    if (in.hofs[i] != h || in.vofs[i] != v || in.mapBase[i] != mb || in.mapSize[i] != ms) same = false;
    in.hofs[i] = h, in.vofs[i] = v, in.mapBase[i] = mb, in.mapSize[i] = ms;
    // (every word mapEntry can read for this layer: up to 64x64 entries from the map's base, wrapping at 32K words)
    const int words = ms == 0 ? 1024 : ms == 3 ? 4096 : 2048;
    for (int k = 0; k < words; k++) {
      const uint16_t w = ppu.vram[(mb + k) & 0x7fff];
      if (in.map[i][k] != w) same = false, in.map[i][k] = w;
    }
  }
  jarInValid_ = true;
  if (!same) jarEdgesWork(ppu);
}

void Smb2Engine::jarEdgesWork(const Ppu& ppu) {
  for (int l = 1; l <= 2; l++) {
    int c0 = ppu.bgHofs(l) >> 3, r0 = ppu.bgVofs(l) >> 3;
    auto at = [&](int c, int r) { return ppu.tileEntry(l, (c0 + c) & 63, (r0 + r) & 63); };
    std::map<uint16_t, int> count;
    for (int r = 0; r < 28; r++)
      for (int c = 0; c < 32; c += 2) count[at(c, r)]++;
    uint16_t blank = 0;
    int best = -1;
    for (auto& kv : count)
      if (kv.second > best) {
        blank = kv.first;
        best = kv.second;
      }
    // (Or the edges meet in pairs of tiles never side by side on the screen:
    // 5-2's jar-shaped room's gravestone, cut at the right edge beside the
    // bushes' tops at the left; repeated round, cut in half at both edges.)
    int n = 0, unmet = 0;
    std::set<uint32_t> pairs;
    for (int r = 0; r < 28; r++)
      for (int c = 0; c < 31; c++) pairs.insert((uint32_t)at(c, r) << 16 | at(c + 1, r));
    for (int r = 0; r < 28; r++) {
      n += (at(0, r) == blank) != (at(31, r) == blank);
      uint16_t a = at(31, r), b = at(0, r);
      unmet += (a != blank || b != blank) && !pairs.count((uint32_t)a << 16 | b);
    }
    jarMirror_[l] = n > 2 || unmet > 2;
    // (An open vertical area's layers scroll up and down: the same over the
    // whole map, rows coming and going at the screen's top and bottom. A
    // walled one's too, behind its walls: 1-3's shaft of light, on BG3,
    // repeated round over the right wall.)
    if (openVertical() || wallTile_[0] >= 0) {
      int m = 0, um = 0;
      std::set<uint32_t> all;
      std::map<uint16_t, int> cnt;
      auto map = [&](int c, int r) { return ppu.tileEntry(l, (c0 + c) & 63, r & 63); };
      for (int r = 0; r < 64; r++)
        for (int c = 0; c < 32; c++) {
          cnt[map(c, r)]++;
          if (c < 31) all.insert((uint32_t)map(c, r) << 16 | map(c + 1, r));
        }
      uint16_t bl = blank;
      int bn = -1;
      for (auto& kv : cnt)
        if (kv.second > bn) bl = kv.first, bn = kv.second;
      for (int r = 0; r < 64; r++) {
        m += (map(0, r) == bl) != (map(31, r) == bl);
        uint16_t a = map(31, r), b = map(0, r);
        um += (a != bl || b != bl) && !all.count((uint32_t)a << 16 | b);
      }
      openMirror_[l] = m > 4 || um > 4;
      openBlank_[l] = bl;
      // (what is joined to each edge column, over the whole map: not things
      // standing alone, 2-1's shaft of light)
      for (int side = 0; side < 2; side++) {
        auto& keep = openKeep_[l][side];
        memset(keep, 0, sizeof(keep));
        std::vector<std::pair<int, int>> todo;
        int edge = side ? 31 : 0;
        for (int r = 0; r < 64; r++)
          if (map(edge, r) != bl) {
            keep[r][edge] = true;
            todo.push_back({r, edge});
          }
        while (!todo.empty()) {
          auto [r, c] = todo.back();
          todo.pop_back();
          const int dr[] = {1, -1, 0, 0}, dc[] = {0, 0, 1, -1};
          for (int k = 0; k < 4; k++) {
            int rr = (r + dr[k]) & 63, cc = c + dc[k];
            if (cc < 0 || cc >= 32 || keep[rr][cc] || map(cc, rr) == bl) continue;
            keep[rr][cc] = true;
            todo.push_back({rr, cc});
          }
        }
      }
    }
    jarBlank_[l] = blank;
    jarRow0_[l] = r0;
    // What is joined to each edge column (4-neighbour fill over non-blank tiles).
    for (int side = 0; side < 2; side++) {
      auto& keep = jarKeep_[l][side];
      memset(keep, 0, sizeof(keep));
      std::vector<std::pair<int, int>> todo;
      int edge = side ? 31 : 0;
      for (int r = 0; r < 28; r++)
        if (at(edge, r) != blank) {
          keep[r][edge] = true;
          todo.push_back({r, edge});
        }
      while (!todo.empty()) {
        auto [r, c] = todo.back();
        todo.pop_back();
        const int dr[] = {1, -1, 0, 0}, dc[] = {0, 0, 1, -1};
        for (int k = 0; k < 4; k++) {
          int rr = r + dr[k], cc = c + dc[k];
          if (rr < 0 || rr >= 28 || cc < 0 || cc >= 32 || keep[rr][cc] || at(cc, rr) == blank) continue;
          keep[rr][cc] = true;
          todo.push_back({rr, cc});
        }
      }
    }
  }
}

int Smb2Engine::jarTile(const Ppu& ppu, int layer, int tx, int ty, uint16_t* entry) const {
  if (layer == 0) return PpuHooks::kWideBlank;
  // (Jars don't scroll these layers: the screen is the map columns from the scroll on.)
  int c0 = ppu.bgHofs(layer) >> 3, d = ((tx - c0) % 64 + 64) % 64;
  if (d >= 48) d -= 64;  // left of the screen: -1, -2, ...; right: 32, 33, ...
  int r = ((ty - jarRow0_[layer]) % 64 + 64) % 64;
  bool flip = jarMirror_[layer];
  // Mirrored, a margin shows what is joined to the edge it is beside;
  // repeated, what is joined to the screen's other edge (which meets it).
  int src = flip ? (d < 0 ? -1 - d : 63 - d) : ((d % 32) + 32) % 32;
  int side = (d < 0) == flip ? 0 : 1;
  bool on = r < 28 && jarKeep_[layer][side][r][src];
  if (!on) {
    *entry = jarBlank_[layer];
    return PpuHooks::kWideEntry;
  }
  uint16_t e = ppu.tileEntry(layer, (c0 + src) & 63, ty);
  *entry = flip ? e ^ 0x4000 : e;
  return PpuHooks::kWideEntry;
}

// An open vertical area's layers in the margins: the map's own columns at
// that row, the screen's edges mirrored or repeated round (see jarEdges).
// (Through a jar's mask, what is joined to an edge on the 28 rows shown,
// the scenery came and went as the screen scrolled up: 5-2's climb.)
int Smb2Engine::openLayerTile(const Ppu& ppu, int layer, int tx, int ty, uint16_t* entry) const {
  int c0 = ppu.bgHofs(layer) >> 3, d = ((tx - c0) % 64 + 64) % 64;
  if (d >= 48) d -= 64;
  bool flip = openMirror_[layer];
  int src = flip ? (d < 0 ? -1 - d : 63 - d) : ((d % 32) + 32) % 32;
  int side = (d < 0) == flip ? 0 : 1;
  // (Beside a frame round the room (BG1, see runOpen_), what is outside it:
  // 6-1's jar-shaped room, its inside's shade on these layers too, met the
  // right edge and went on. Taken from the nearest rows on screen clear of
  // the frame (the cave above or below it; the rows further off aren't
  // streamed in yet), 32 px off so the pattern keeps in step.)
  int edge = d < 0 ? 0 : 1, camY = ram(0x00ca) << 8 | ram(0x00cb);
  auto framed = [&](int row) {
    int sy = ((row * 8 - ppu.bgVofs(layer)) % 512 + 512) % 512;
    if (sy >= 224) return -1;  // (off screen)
    int ar = (camY + sy) / 16;
    return ar < 160 && runOpen_[edge][ar] ? 1 : 0;
  };
  if (framed(ty) == 1) {
    int from = -1;
    for (int k = 4; k < 32 && from < 0; k += 4)
      for (int r : {ty + k, ty - k})
        if (from < 0 && framed(r) == 0) from = r;
    if (from < 0) {
      *entry = openBlank_[layer];
      return PpuHooks::kWideEntry;
    }
    // (mirrored at its own edge: repeated round, the other edge's columns
    // are the room's inside)
    uint16_t e = ppu.tileEntry(layer, (c0 + (d < 0 ? -1 - d : 63 - d)) & 63, from);
    *entry = e ^ 0x4000;
    return PpuHooks::kWideEntry;
  }
  if (!openKeep_[layer][side][ty & 63][src]) {
    *entry = openBlank_[layer];
    return PpuHooks::kWideEntry;
  }
  uint16_t e = ppu.tileEntry(layer, (c0 + src) & 63, ty);
  *entry = flip ? e ^ 0x4000 : e;
  return PpuHooks::kWideEntry;
}

int Smb2Engine::fetchWideTile(Ppu& ppu, int layer, int tx, int ty, int screenX, uint16_t* entry) {
  if (layer > 2) return PpuHooks::kWideVram;
  if (jar_) return jarTile(ppu, layer, tx, ty, entry);
  // (A one-screen vertical room: its walls carried on as an open vertical
  // area's (2-3's key room, a frame of two pieces); a jar's shape, blank at
  // the edges, stands in the layers behind as a jar's room.)
  if (roomJar_) {
    if (layer == 0) return screenX >= 16 && screenX < 240 ? PpuHooks::kWideVram : openVerticalTile(tx, ty, screenX, entry, false);
    if (screenX >= 0 && screenX < 256) return PpuHooks::kWideVram;
    return jarTile(ppu, layer, tx, ty, entry);
  }
  if (oneRoomH_) {
    if (layer == 0) return screenX >= 16 && screenX < 240 ? PpuHooks::kWideVram : openVerticalTile(tx, ty, screenX, entry, true);
    if (screenX >= 0 && screenX < 256) return PpuHooks::kWideVram;
    return jarTile(ppu, layer, tx, ty, entry);
  }
  if (subspace_) {
    // Subspace is a copy of the screen it was entered from, turned round, on
    // page 10 ($12:F394: from the camera then, $0516:$050B, rounded to 16 px,
    // + 8). Beside it: the level beside that screen, turned round the same
    // way (each tile's halves swapped and flipped, $14F33C). Only BG1 is on.
    if (layer != 0) return PpuHooks::kWideBlank;
    int src = ((ram(0x0516) << 8 | ram(0x050b)) + 8) & ~15;
    // (past the area's ends, entered near one: its end column carried on,
    // not blank, 2-3's cave wall)
    int lx = src + 255 - screenX, end = (ram(0x0541) + 1) * 256;
    if (lx < 0) lx &= 15;
    else if (lx >= end) lx = end - 16 + (lx & 15);
    if (!bg1Entry(lx, (ty & 63) * 8, entry)) return PpuHooks::kWideBlank;
    *entry ^= 0x4000;
    return PpuHooks::kWideEntry;
  }
  if (!horizontal_) {
    // Vertical areas are 256px wide and wrap around horizontally. One walled
    // both sides all the way up (see findWalledVertical): its walls carry on
    // over the margins (BG1); behind them the layers as they wrap.
    // (Their edge columns too, in the game's own 256 px: the edge tile is
    // drawn for the screen's edge, narrower bricks outlined on its outer
    // side; inside a wall carried on, it stood out as a seam.)
    if (layer == 0 && wallTile_[0] >= 0) {
      if (screenX >= 16 && screenX < 240) return PpuHooks::kWideVram;
      int side = screenX < 128 ? 0 : 1;
      *entry = metaEntry(wallTile_[side], tx & 1, ty & 1);
      return PpuHooks::kWideEntry;
    }
    if (openVertical()) {
      if (layer == 0) return screenX >= 16 && screenX < 240 ? PpuHooks::kWideVram : openVerticalTile(tx, ty, screenX, entry, false);
      if (screenX >= 0 && screenX < 256) return PpuHooks::kWideVram;
      return openLayerTile(ppu, layer, tx, ty, entry);
    }
    if (wallTile_[0] >= 0 && (screenX < 0 || screenX >= 256)) return openLayerTile(ppu, layer, tx, ty, entry);
    *entry = ppu.tileEntry(layer, tx & 31, ty);
    return PpuHooks::kWideEntry;
  }
  // Horizontal scroll registers are (scroll XOR $100): map column tx holds
  // data column (tx ^ 32); pick the one nearest the pixel's expected position.
  int want = ((layer ? layerAbs_[layer] + bgAdj_[layer] : cam16_) + screenX) >> 3;
  int mc = (tx ^ 32) & 63;
  int col = want + (((mc - want) % 64 + 64 + 32) % 64 - 32);
  int x = col * 8, y = (ty & 63) * 8;
  if (layer == 1 && ram(0x024b) && y < 80) {
    // Star-sky areas ($024B) only stream BG2 tile rows 10-29 ($13F295): the
    // sky above is drawn once at area load, so VRAM is what the game shows.
    return PpuHooks::kWideVram;
  }
  bool ok = layer == 0 ? bg1Entry(x, y, entry) : bgLayerEntry(layer, x, y, entry);
  return ok ? PpuHooks::kWideEntry : PpuHooks::kWideBlank;
}

std::string Smb2Engine::verify(int layer) {
  const Ppu& ppu = snes_.ppu;
  int hofs = ppu.bgHofs(layer), vofs = ppu.bgVofs(layer);
  int n = 0, bad = 0;
  for (int row = 0; row < 224; row += 8) {
    for (int sx = 0; sx < 256; sx += 8) {
      int tx = ((sx + hofs) & 0x3ff) >> 3;
      int ty = ((row + 1 + vofs) & 0x3ff) >> 3;
      uint16_t v = ppu.tileEntry(layer, tx, ty), e = 0;
      int r = fetchWideTile(const_cast<Ppu&>(ppu), layer, tx, ty, sx, &e);
      if (r != PpuHooks::kWideEntry) continue;
      n++;
      if (e != v) bad++;
    }
  }
  char buf[64];
  snprintf(buf, sizeof buf, "layer %d: %d compared, %d mismatched", layer + 1, n, bad);
  return buf;
}

// ---------------------------------------------------------------------------
// Cheats

void Smb2Engine::applyCheats(uint32_t f) {
  if (!f) return;
  uint8_t* w = snes_.wram;
  if ((f & kCheatLives) && w[0x04ee] < 9) w[0x04ee] = 9;
  if (!gameplay_) return;
  if (f & kCheatHearts) w[0x04c4] = 2;
  if (f & kCheatHealth) {
    int hearts = w[0x04c4] + 2;
    w[0x04c3] = (uint8_t)(((hearts - 1) << 4) | 0x0f);
  }
  if ((f & kCheatStar) && w[0x04e1] < 0x20) w[0x04e1] = 0x20;
  if (f & kCheatJump) {
    // Character jump speeds: standing, carrying, running (x2)
    w[0x054e] = w[0x054f] = w[0x0552] = w[0x0553] = 0x90;
  }
  if (f & kCheatFloat) w[0x0555] = 0x3c;
}

// ---------------------------------------------------------------------------
// Level select (docs/re/smb2.md §3). From the title screen: set level/world,
// press Start, then A on the character select. During gameplay the game's
// death/restart path is reused so no reset is needed.

bool Smb2Engine::atTitle() const {
  return ram(0x0100) == 0x40 && ram(0x0775) == 2 && ram(0x0217) == 0 && ram(0x0789) == 0;
}

bool Smb2Engine::inGame() const { return gameplay_; }

void Smb2Engine::requestWarp(int world, int level, int character) {
  warpWorld_ = std::clamp(world, 0, 6);
  int maxLevel = kWorldFirstLevel[warpWorld_ + 1] - kWorldFirstLevel[warpWorld_] - 1;
  warpLevel_ = std::clamp(level, 0, maxLevel);
  warpChar_ = character >= 0 && character < 4 ? character : -1;
  warpStage_ = 1;
  warpTimer_ = 0;
  warpSeenSelect_ = false;
  message_.clear();
}

std::string Smb2Engine::takeMessage() {
  std::string m;
  m.swap(message_);
  return m;
}

void Smb2Engine::updateWarp() {
  input_ = 0;
  if (!warpStage_) return;
  uint8_t* w = snes_.wram;
  int t = warpTimer_++;
  uint8_t level = (uint8_t)(kWorldFirstLevel[warpWorld_] + warpLevel_);
  auto next = [&](int s) {
    warpStage_ = s;
    warpTimer_ = 0;
  };
  // (NMI mode is $40 for the character select after the title, $C0 after a death.)
  bool charSelect = ram(0x0217) == 1 && ram(0x0775) == 2;
  switch (warpStage_) {
    case 1:
      if (atTitle()) {
        if (t > 30) next(2);
      } else if (gameplay_) {
        next(10);
      } else if (t > 900) {
        warpStage_ = 0;
        message_ = "Return to the SMB2 title screen to use level select";
      }
      break;
    case 2:  // title screen: choose the level, then Start
      w[0x0533] = level;
      w[0x0635] = (uint8_t)warpWorld_;
      if (t >= 4 && t < 7) input_ = kPadStart;
      if (t > 10) next(3);
      break;
    case 3:  // character select: confirm the highlighted character (the chosen one, if any)
      if (charSelect) {
        warpSeenSelect_ = true;
        if (warpChar_ >= 0) w[0x008f] = (uint8_t)warpChar_;
        // Keep pressing A (briefly) until the game accepts it.
        if (t > 60 && t % 40 < 3) input_ = kPadA;
      } else if (warpSeenSelect_ && gameplay_) {
        // Arrived. A new game doesn't set the restart checkpoint (it assumes
        // 1-1), so a death would send the player back there: set it.
        for (int i = 0; i < 4; i++) w[0x04e8 + i] = w[0x0533 + i];
        warpStage_ = 0;
      }
      if (t > 2400) {
        warpStage_ = 0;
        message_ = "Level select could not start the level";
      }
      break;
    case 10:  // mid-game: restart through the death path at the new level
      w[0x04e8] = level;
      w[0x04e9] = w[0x04ea] = w[0x04eb] = 0;
      w[0x0533] = level;
      w[0x0635] = (uint8_t)warpWorld_;
      if (w[0x04ee] < 99) w[0x04ee]++;  // the restart costs a life
      w[0x0050] = 7;                      // start dying
      next(3);
      break;
  }
}

std::string Smb2Engine::debug() const {
  char buf[256];
  snprintf(buf, sizeof buf,
           "nmi=%02X area=%d horiz=%d cam=%d ext=%d+%d warp=%d bg=%d/%d abs=%d/%d kinds=%d,%d,%d/%d,%d,%d "
           "edge=%d%c/%d%c",
           ram(0x0100), ram(0x0534), horizontal_, cam16_, extL_, extR_, warpStage_, bgAdj_[1], bgAdj_[2], layerAbs_[1],
           layerAbs_[2], bandKind_[1][0], bandKind_[1][1], bandKind_[1][2], bandKind_[2][0], bandKind_[2][1],
           bandKind_[2][2], layerLoop_[1], mirrorStart_[1] ? 'm' : '-', layerLoop_[2], mirrorStart_[2] ? 'm' : '-');
  return buf;
}

// The ending's parade, shown wide (SmasGame): Wart's sprites at their own
// positions (recorded as drawn last frame, in OAM now), the rest as they
// are; his exit and its stars past the view's edge (extR 0: the game's own).
void Smb2Engine::paradeSprites(Ppu& ppu, int extR) {
  for (int i = 0; i < 128; i++) {
    ppu.objTrueXSet[i] = paradeSet_[i];
    if (paradeSet_[i]) ppu.objTrueX[i] = paradeX_[i];
    paradeSet_[i] = false;
  }
  trueX_ = true;
  paradeExtR_ = extR;
  if (extR > 0) {
    patch(kParadeExit, (uint8_t)(0x20 + extR));
  } else {
    auto it = original_.find(kParadeExit);
    if (it != original_.end()) snes_.poke(kParadeExit, it->second);
  }
}

// The parade begins with Wart at x -96 ($12:9469: LDA #$FF / STA $14, LDA
// #$A0 / STA $28), off the 4:3 screen, carried in; inside the wide view's
// margin he appeared there. Past its edge instead, all of him (48 px wide),
// for as long as the ending is shown wide (extL 0: the game's own).
void Smb2Engine::paradeStart(int extL) {
  if (extL > 0) {
    int x = std::min(-96, -extL - 48);
    patch(0x12946a, (uint8_t)((x >> 8) & 0xff));
    patch(0x12946e, (uint8_t)(x & 0xff));
  } else {
    for (uint32_t a : {0x12946au, 0x12946eu}) {
      auto it = original_.find(a);
      if (it != original_.end()) snes_.poke(a, it->second);
    }
  }
}
