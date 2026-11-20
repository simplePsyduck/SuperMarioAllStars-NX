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
// Not in co-op (Luigi's touches are run with his variables in: one player's).
#include <algorithm>
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
}

bool Smb1Engine::carryExec(uint32_t pc) {
  CpuRegs& r = snes_.cpu.r;
  const int x = r.x & 0xff;
  if (pc == carryTouch_) {
    if (x != carried_) return false;
    r.pc = (uint16_t)carryExit_;  // (the one held: no kick, no hurt)
    return true;
  }
  if (pc == carryKick_) {
    // A still shell about to be kicked: with run held, picked up instead.
    if (!(cheats_ & kCheatCarry) || coop_ || carried_ >= 0 || !(ram(0x000A) & 0x40) || x > 4) return false;
    carried_ = x;
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
  const int py = ram(0x00BB) * 256 + ram(0x0237) + (ram(0x0754) == 0 ? 10 : 0);  // (big: at his middle)
  w[0x021A + x] = (uint8_t)sx;
  w[0x0079 + x] = (uint8_t)(sx >> 8);
  w[0x0238 + x] = (uint8_t)py;
  w[0x00BC + x] = (uint8_t)(py >> 8);
  w[0x005E + x] = 0;  // (X speed)
  w[0x00A1 + x] = 0;  // (Y speed)
  if (w[0x07A2 + x] < 0x10) w[0x07A2 + x] = 0x10;  // (not coming out of it while held)
}

void Smb1Engine::carryFrame() {
  if (carried_ < 0) return;
  const int x = carried_;
  const uint8_t flag = ram(0x0010 + x), id = ram(0x001C + x), state = ram(0x0029 + x), sub = ram(0x000F);
  // (GameEngineSubroutine: 8 normal play, 9 growing, $0A shrinking, $0C the fire flower's flashing; any
  // other (a pipe, the flagpole, dying...) lets go)
  const bool playing = ram(0x0770) == 1 && (sub == 8 || sub == 9 || sub == 0x0A || sub == 0x0C);
  if (!(cheats_ & kCheatCarry) || coop_ || !playing || !flag || (flag & 0x80) || id > 3 || (state & 0x80) ||
      (state & 7) < 2) {
    carried_ = -1;
    return;
  }
  if (sub == 8 && !(ram(0x000A) & 0x40)) {
    // Run let go: just in front of the player, touching him: the game's own touch kicks it, away
    // from him (its smack, its speed, its points).
    carryPlace();
    const bool right = ram(0x0202) == 1;
    const int px = ram(0x0078) * 256 + ram(0x0219);
    const int sx = px + (right ? 8 : -8);
    snes_.wram[0x021A + x] = (uint8_t)sx;
    snes_.wram[0x0079 + x] = (uint8_t)(sx >> 8);
    // (a new touch: the one that picked it up is still marked, and a touch marked from the last is
    // left alone)
    snes_.wram[carryTouchBits_ + x] &= (uint8_t)~1;
    carried_ = -1;
  }
}
