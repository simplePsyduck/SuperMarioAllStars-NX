// Pick Up & Carry Koopa Shells (Cheats; Super Mario Bros. and The Lost Levels),
// as in Super Mario Bros. 3: hold the run button (Y) and touch a still shell to
// pick it up; it's carried in front of the player; let go of run to throw it.
//
// The game's own touch of an enemy (PlayerEnemyCollision, SMB1 $03:DE90, LL
// $0D:DDBE: X the enemy, after PlayerCollisionCore found them touching) kicks a
// still shell (Koopas and Buzzy Beetles, ID 0-3, [Enemy_State] $29,X 2-7) from
// $03:DEE7: the smack sound, bit 7 of its state, its speed away from the
// player, the points. There, with run held, it's picked up instead; and while
// it's held its touches are left out (nothing kicks it, nor hurts the player).
// Each frame, as the enemy's own update is about to run (enemyCall_), it's put
// beside the player, still: the game moves and draws it from there. Its revival
// timer ($07A2,X) is held up meanwhile. Let go of run and it's put just in front
// of the player, touching, its touch mark ([Enemy_CollisionBits] bit 0, set by the touch that picked it
// up) cleared: the game's own touch kicks it, away from him.
//
// While it's held it's marked moving ([Enemy_State] bit 7, as a kicked shell): the game's own
// enemy-to-enemy touches (EnemiesCollision) then knock out what it bumps into, with the smack
// and the chain of points a sliding shell gets (and a sliding shell it meets knocks it out too, as
// in Super Mario Bros. 3: see below); its speed is held at 0, so it still only goes where the player takes it.
// The mark comes off as it's let go (the kick wants a still shell) or dropped: its state as it was
// picked up is put back (the game adds bit 6 to a moving shell's too).
//
// While held, the first enemy it bumps into is knocked out and so is the shell (Super Mario Bros.
// 3's one hit): the knock-out (ShellOrBlockDefeat) is watched for the held shell's touch.
//
// The throw: the kick sends it off at the speed of a kicked shell (3 px a frame); a throw is a
// little faster, 3.25 px, up to 3.75 px with the player's own run speed in that direction, and half
// a pixel more thrown in the air (it falls as it goes, so it carries further), as in Super Mario Bros. 3.
//
// Not in co-op (Luigi's touches are run with his variables in: one player's).
#include <algorithm>
#include <cstdlib>
#include "core/snes.h"
#include "game/smb1.h"

void Smb1Engine::carryInit(uint8_t bankLo, uint8_t bankHi) {
  // The kick: LDA $29,X; ORA #$80; STA $29,X (the shell moving); its start, LDA #$03; STA $1600 (the
  // smack), 12 bytes before; the routine's RTS 33 bytes after. The touch: LDX $9E; BCS (to it), at most
  // 256 bytes before.
  static const uint16_t kSigKick[] = {0xB5, 0x29, 0x09, 0x80, 0x95, 0x29};
  carryTouch_ = carryKick_ = carryExit_ = 0;
  const uint32_t k = findSig(kSigKick, sizeof(kSigKick) / 2, bankLo, bankHi);
  if (!k) return;
  if (snes_.peek(k - 12) != 0xA9 || snes_.peek(k - 11) != 0x03 || snes_.peek(k - 10) != 0x8D ||
      snes_.peek(k + 0x21) != 0x60)
    return;
  for (uint32_t a = k - 0x100; a < k; a++)
    if (snes_.peek(a) == 0xA6 && snes_.peek(a + 1) == 0x9E && snes_.peek(a + 2) == 0xB0) {
      carryTouch_ = (uint32_t)((int)(a + 4) + (int8_t)snes_.peek(a + 3));
      break;
    }
  if (!carryTouch_) return;
  // ([Enemy_CollisionBits], set as a touch is handled: LDA #$01; ORA abs,X; STA abs,X; a touch while it's
  // set from the last is left alone)
  carryTouchBits_ = 0;
  for (uint32_t a = carryTouch_; a < k; a++)
    if (snes_.peek(a) == 0xA9 && snes_.peek(a + 1) == 0x01 && snes_.peek(a + 2) == 0x1D && snes_.peek(a + 5) == 0x9D &&
        snes_.peek(a + 3) == snes_.peek(a + 6) && snes_.peek(a + 4) == snes_.peek(a + 7)) {
      carryTouchBits_ = (uint16_t)(snes_.peek(a + 3) | snes_.peek(a + 4) << 8);
      break;
    }
  if (!carryTouchBits_) {
    carryTouch_ = 0;
    return;
  }
  carryKick_ = k - 12;
  carryExit_ = k + 0x21;
  // (the knock-out of ShellOrBlockDefeat: SMB1 $03:DF92, LL $0D:DEB4)
  static const uint16_t kSigKnock[] = {0xB5, 0x29, 0x29, 0x1F, 0x09, 0x20, 0x95, 0x29};
  carryKnock_ = findSig(kSigKnock, sizeof(kSigKnock) / 2, bankLo, bankHi);
  // EnemiesCollision leaves piranha plants out, the enemy's and the other's (CMP #$15; BCS; CMP #Lakitu;
  // BEQ; CMP #PiranhaPlant; BEQ: SMB1 $03:E0D7 and $03:E0F8, LL $0D:E016 and $0D:E03B)
  static const uint16_t kSigPir[] = {0xC9, 0x15, 0xB0, 0x100, 0xC9, 0x11, 0xF0, 0x100, 0xC9, 0x0D, 0xF0};
  pirOuter_ = pirInner_ = 0;
  if (uint32_t a = findSig(kSigPir, sizeof(kSigPir) / 2, bankLo, bankHi)) {
    for (uint32_t b = a + 1; b < a + 0x60 && !pirInner_; b++) {
      bool ok = true;
      for (size_t k = 0; k < sizeof(kSigPir) / 2 && ok; k++) ok = kSigPir[k] == 0x100 || snes_.peek(b + k) == kSigPir[k];
      if (ok) pirInner_ = b + 8;
    }
    if (pirInner_) pirOuter_ = a + 8;
  }
}

// A sliding shell (a Koopa's or Buzzy Beetle's, kicked: [Enemy_State] bit 7; a held one is marked so too)
bool Smb1Engine::shellMoving(int x) const {
  return x >= 0 && x < 12 && ram(0x0010 + x) && ram(0x001C + x) <= 3 && (ram(0x0029 + x) & 0x80) && !(ram(0x0029 + x) & 0x20);
}

bool Smb1Engine::carryExec(uint32_t pc) {
  CpuRegs& r = snes_.cpu.r;
  const int x = r.x & 0xff;
  if (pc == carryTouch_) {
    if (x != carried_) return false;
    r.pc = (uint16_t)carryExit_;  // (the one held: no kick, no hurt)
    return true;
  }
  if (pc == pirOuter_ || pc == pirInner_) {
    // Piranha plants and sliding shells (with this cheat on, as in Super Mario Bros. 3: a kicked or
    // thrown shell knocks one out). The game leaves plants out of its enemy-to-enemy touches: a plant
    // is let in where the other of the pair is a sliding shell (the plant's own turn: if any enemy
    // before it is one; then each other it meets, only a sliding shell). The touch then knocks the plant
    // out as it does any enemy a shell hits (ShellOrBlockDefeat, which lifts a plant by 24 px first).
    if (!(cheats_ & kCheatCarry) || coop_) return false;
    const int id = r.a & 0xff;
    const int skip = 4;  // (CMP #$0D; BEQ: on as if it weren't a plant)
    if (pc == pirOuter_) {
      if (id != 0x0D) return false;
      bool any = false;
      for (int k = 0; k < x && !any; k++) any = shellMoving(k);
      if (!any) return false;
      r.pc = (uint16_t)(r.pc + skip);
      return true;
    }
    const int outer = ram(0x009E);
    const bool outerPlant = outer < 12 && ram(0x001C + outer) == 0x0D;
    if (id == 0x0D) {
      if (!shellMoving(outer)) return false;
      r.pc = (uint16_t)(r.pc + skip);
      return true;
    }
    if (outerPlant && !shellMoving(x)) {
      // (the plant let in meets something else: left out, as the game leaves plants: its BEQ's target)
      r.pc = (uint16_t)(r.pc + skip + (int8_t)snes_.peek(((uint32_t)r.pb << 16) | (uint16_t)(r.pc + 3)));
      return true;
    }
    return false;
  }
  if (pc == carryKnock_) {
    // An enemy knocked out by the held shell (the enemy-to-enemy touch: one of the pair, ObjectOffset
    // $9E or $01, is the shell, and the two are where the touch put them): the shell is knocked out
    // with it, as in Super Mario Bros. 3: one hit.
    if (carried_ < 0 || x == carried_ || (ram(0x009E) != carried_ && ram(0x0001) != carried_)) return false;
    const int c = carried_;
    const int ex = ram(0x0079 + x) * 256 + ram(0x021A + x), cx = ram(0x0079 + c) * 256 + ram(0x021A + c);
    const int ey = ram(0x00BC + x) * 256 + ram(0x0238 + x), cy = ram(0x00BC + c) * 256 + ram(0x0238 + c);
    if (std::abs(ex - cx) > 24 || std::abs(ey - cy) > 24) return false;
    uint8_t* w = snes_.wram;
    w[0x0029 + c] = (uint8_t)((carriedState_ & 0x1f) | 0x20);  // (as the knock-out does: falling off the screen)
    w[0x00A1 + c] = 0xFD;                                          // (a hop up first)
    w[0x005E + c] = (uint8_t)(ram(0x0202) == 1 ? 0x08 : 0xF8);     // (drifting the way the player faces)
    carried_ = -1;
    return false;
  }
  if (pc == carryKick_) {
    // A still shell about to be kicked: with run held, picked up instead.
    if (!(cheats_ & kCheatCarry) || coop_ || carried_ >= 0 || !(ram(0x000A) & 0x40) || x > 4) return false;
    carried_ = x;
    thrown_ = -1;
    carriedState_ = snes_.wram[0x0029 + x];
    snes_.wram[0x0029 + x] |= 0x80;  // (marked moving while held: see above)
    r.pc = (uint16_t)carryExit_;
    return true;
  }
  return false;
}

// The held shell's update is about to run: beside the player, still.
void Smb1Engine::carryPlace() {
  uint8_t* w = snes_.wram;
  const int x = carried_;
  const bool right = ram(0x0202) == 1;  // [PlayerFacingDir]
  const int px = ram(0x0078) * 256 + ram(0x0219);
  const int sx = px + (right ? 11 : -11);
  // (held out in front, a little up off the ground: small, 4 px up; big, at his middle, 6 px down)
  const int py = ram(0x00BB) * 256 + ram(0x0237) + (ram(0x0754) == 0 ? 6 : -4);
  w[0x021A + x] = (uint8_t)sx;
  w[0x0079 + x] = (uint8_t)(sx >> 8);
  w[0x0238 + x] = (uint8_t)py;
  w[0x00BC + x] = (uint8_t)(py >> 8);
  w[0x005E + x] = 0;  // (X speed)
  w[0x00A1 + x] = 0;  // (Y speed)
  if (w[0x07A2 + x] < 0x10) w[0x07A2 + x] = 0x10;  // (not coming out of it while held)
}

void Smb1Engine::carryFrame() {
  uint8_t* w = snes_.wram;
  if (thrown_ >= 0) {
    // (the kick of the shell just let go: once it's sliding, away from the player, at the throw's speed)
    const int x = thrown_;
    const uint8_t state = ram(0x0029 + x);
    if (!ram(0x0010 + x) || ram(0x001C + x) > 3 || (state & 0x20) || --throwWait_ <= 0) {
      thrown_ = -1;
    } else if (state & 0x80) {
      const int8_t v = (int8_t)ram(0x005E + x);
      if (v == 0x30 || v == -0x30) w[0x005E + x] = (uint8_t)(v > 0 ? throwSpeed_ : -throwSpeed_);
      thrown_ = -1;
    }
  }
  if (carried_ < 0) return;
  const int x = carried_;
  const uint8_t flag = ram(0x0010 + x), id = ram(0x001C + x), state = ram(0x0029 + x), sub = ram(0x000F);
  // (GameEngineSubroutine: 8 normal play, 9 growing, $0A shrinking, $0C the fire flower's flashing; any
  // other (a pipe, the flagpole, dying...) lets go)
  const bool playing = ram(0x0770) == 1 && (sub == 8 || sub == 9 || sub == 0x0A || sub == 0x0C);
  // (gone, not a shell, knocked out (bit 5: a sliding shell or a fireball got it; the knock-out takes
  // its moving mark off), or no longer a still shell under the mark)
  if (!flag || (flag & 0x80) || id > 3 || (state & 0x20) || (state & 7) < 2) {
    carried_ = -1;
    return;
  }
  if (!(cheats_ & kCheatCarry) || coop_ || !playing) {
    w[0x0029 + x] = carriedState_;  // (dropped where it is: a still shell again)
    carried_ = -1;
    return;
  }
  if (sub == 8 && !(ram(0x000A) & 0x40)) {
    // Run let go: just in front of the player, touching him, a still shell again: the game's own touch
    // kicks it, away from him (its smack, its speed, its points); then it's sped up to the throw's.
    carryPlace();
    const bool right = ram(0x0202) == 1;
    const int px = ram(0x0078) * 256 + ram(0x0219);
    const int sx = px + (right ? 8 : -8);
    w[0x021A + x] = (uint8_t)sx;
    w[0x0079 + x] = (uint8_t)(sx >> 8);
    // (down at his feet, as a shell he stands beside: held higher, his touch missed it)
    const int py = ram(0x00BB) * 256 + ram(0x0237) + (ram(0x0754) == 0 ? 10 : 0);
    w[0x0238 + x] = (uint8_t)py;
    w[0x00BC + x] = (uint8_t)(py >> 8);
    w[0x0029 + x] = carriedState_;
    // (a new touch: the one that picked it up is still marked, and a touch marked from the last is
    // left alone)
    w[carryTouchBits_ + x] &= (uint8_t)~1;
    const int run = (int8_t)ram(0x005D) * (right ? 1 : -1);  // [Player_X_Speed], toward the throw
    const bool air = ram(0x0028) != 0;                         // [Player_State]: jumping or falling
    throwSpeed_ = std::clamp(0x34 + std::max(run, 0) / 4, 0x34, 0x3C) + (air ? 0x08 : 0);
    thrown_ = x;
    throwWait_ = 8;
    carried_ = -1;
  }
}
