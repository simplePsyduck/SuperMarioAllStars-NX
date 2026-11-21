// 1P Luigi game (Super Mario Bros. only): the title's first choice reads 1P MARIO GAME, and Left or
// Right turns it to 1P LUIGI GAME (and back). That starts the usual one-player game as Luigi: the
// game's own player 2 ([CurrentPlayer] $0753 1: his name over the score, his colours, which the game
// already has for its 2 PLAYER GAME), with The Lost Levels' Luigi: his higher, floatier jump and his
// looser footing. The Lost Levels picks those from its tables ($0D:9A77: Mario's, then Luigi's) as a
// jump starts and as the player's speed is worked out; Super Mario Bros. has Mario's only ($03:B481).
// The game's own sprite upload already has Luigi's own drawings (his half of bank $0A, $C000 on, the
// ones The Lost Levels shows: not Mario recoloured) behind a flag, $0EC2, that only its 2 PLAYER GAME
// sets: set here too. And as The Lost Levels does ($0D:B38E) his friction isn't doubled as he turns
// about. Its three stores of the physics are watched and Luigi's value put in instead:
//
//   JumpMForceData  (STA $0709, the pull down while A is held)  Mario 20 20 1E 28 28 0D 04
//                                                               Luigi 18 18 18 22 22 0D 04
//   FallMForceData  (STA $070A, the pull down otherwise)        Mario 70 70 60 90 90 0A 09
//                                                               Luigi 42 42 3E 5D 5D 0A 09
//   FrictionData    (STA $0702, how fast he turns and stops)    Mario E4 98 D0
//                                                               Luigi B4 68 A0
//
// Everything else (power-ups, the saves, Save Level Progress' power-up) is the game's as ever: they
// belong to the file, not the brother, so a Mario save's Fire Mario loads as Fire Luigi and back.
// The choice is kept until the title comes up again; not with a 2 PLAYER GAME. With co-op turned on
// (Mario joining on the second controller) it goes co-op's way: the game's player is its Mario again
// ([CurrentPlayer] 0, the first controller), shown and moved as Luigi; Mario the other (see p2Sync).
#include "core/snes.h"
#include "game/smb1.h"

namespace {
const uint8_t kLuigiJump[7] = {0x18, 0x18, 0x18, 0x22, 0x22, 0x0D, 0x04};
const uint8_t kLuigiFall[7] = {0x42, 0x42, 0x3E, 0x5D, 0x5D, 0x0A, 0x09};
const uint8_t kLuigiFriction[3] = {0xB4, 0x68, 0xA0};
}  // namespace

void Smb1Engine::luigiInit(uint8_t bankLo, uint8_t bankHi) {
  luigiJump_ = luigiFall_ = luigiFriction_ = 0;
  if (lostLevels_) return;  // (The Lost Levels has its own LUIGI GAME)
  // LDA JumpMForceData,Y; STA $0709; LDA FallMForceData,Y; STA $070A (their stores)
  static const uint16_t kSigJump[] = {0xB9, 0x100, 0x100, 0x8D, 0x09, 0x07, 0xB9, 0x100, 0x100, 0x8D, 0x0A, 0x07};
  if (uint32_t a = findSig(kSigJump, sizeof(kSigJump) / 2, bankLo, bankHi)) {
    luigiJump_ = a + 3;
    luigiFall_ = a + 9;
  }
  // LDY $00; LDA FrictionData,Y; STA $0702
  static const uint16_t kSigFriction[] = {0xA4, 0x00, 0xB9, 0x100, 0x100, 0x8D, 0x02, 0x07};
  if (uint32_t a = findSig(kSigFriction, sizeof(kSigFriction) / 2, bankLo, bankHi)) luigiFriction_ = a + 5;
  // LDA $0202; CMP $46; BEQ; ASL $0702; ROL $0701 (turning about: the friction doubled. The Lost Levels
  // skips that for Luigi, $0D:B38E: LDA $0EC2; BNE)
  static const uint16_t kSigTurn[] = {0xAD, 0x02, 0x02, 0xC5, 0x46, 0xF0, 0x100, 0x0E, 0x02, 0x07, 0x2E, 0x01, 0x07};
  luigiTurn_ = 0;
  if (uint32_t a = findSig(kSigTurn, sizeof(kSigTurn) / 2, bankLo, bankHi)) luigiTurn_ = a + 7;
}

bool Smb1Engine::luigiExec(uint32_t pc) {
  // (Who's moving is Luigi: alone, [CurrentPlayer] 1; in co-op, the game's player if he's Luigi, else
  // the other on his turn)
  const bool luigiNow = coop_ ? (coopMove_ != 0 ? !luigiLive_ : luigiLive_) : ram(0x0753) == 1;
  if (!luigiGame_ || !luigiNow) return false;
  CpuRegs& r = snes_.cpu.r;
  if (pc == luigiTurn_) {
    r.pc = (uint16_t)(r.pc + 6);  // (his friction not doubled as he turns about)
    return true;
  }
  const int y = r.y & 0xff;
  int v = -1;
  if (pc == luigiJump_ && y < 7) v = kLuigiJump[y];
  else if (pc == luigiFall_ && y < 7) v = kLuigiFall[y];
  else if (pc == luigiFriction_ && y < 3) v = kLuigiFriction[y];
  if (v < 0) return false;
  r.a = (uint16_t)((r.a & 0xff00) | v);  // (the store then writes Luigi's; its flags aren't looked at)
  return false;
}

// Each frame, before it: a Luigi game started is Luigi's ([CurrentPlayer] 1; the game puts it to 0 as
// it sets a game up, a continue included), until the title is back; his buttons the first controller's.
void Smb1Engine::luigiFrame() {
  if (lostLevels_ || !luigiGame_) return;
  const uint8_t mode = ram(0x0770);
  if (mode == 0) return;  // (titleFrame decides again as the title comes up)
  if (ram(0x077A) != 0) {
    luigiGame_ = false;  // (not a one-player game)
    return;
  }
  if (coop_) {
    luigiCoop_ = true;  // (co-op's way: see p2Sync; its controllers as they are)
    return;
  }
  if (luigiCoop_) {
    // (co-op over: on as Luigi if he's the game's player, else it's Mario's game now)
    luigiCoop_ = false;
    if (!luigiLive_) {
      luigiGame_ = false;
      snes_.wram[0x0EC2] = 0;
      return;
    }
    luigiLive_ = false;
  }
  if (ram(0x0753) != 1) snes_.wram[0x0753] = 1;
  if (ram(0x0EC2) != 1) snes_.wram[0x0EC2] = 1;  // (his own sprites: the player's tiles from his half, $0A:C000)
  // (the game reads player 2's buttons from the second controller: Luigi is played from the first)
  snes_.input[1] = snes_.input[0];
}

// The first choice's words, 1P MARIO GAME or 1P LUIGI GAME, over the game's 1 PLAYER GAME (13 tiles
// either way, from the same column, in its colours).
void Smb1Engine::luigiTitleText() {
  if (lostLevels_ || !titleInput_) return;
  // (the title's font: 0-9, A-Z from $0A, blank $24)
  static const uint8_t kMario[13] = {0x01, 0x19, 0x24, 0x16, 0x0A, 0x1B, 0x12, 0x18, 0x24, 0x10, 0x0A, 0x16, 0x0E};
  static const uint8_t kLuigi[13] = {0x01, 0x19, 0x24, 0x15, 0x1E, 0x12, 0x10, 0x12, 0x24, 0x10, 0x0A, 0x16, 0x0E};
  uint16_t* vram = snes_.ppu.vram;
  const uint16_t row1 = 0x024a;  // (kTitleSmb1.row1: see smb1coop.cpp)
  const uint8_t* t = luigiPick_ ? kLuigi : kMario;
  const uint16_t attr = vram[row1] & 0xfc00;
  for (int i = 0; i < 13; i++) vram[row1 + i] = (uint16_t)(attr | t[i]);
}

// Each frame, after it (titleFrame): the title's first row up (its 1 and the P of PLAYER, or of our
// 1P), Left or Right pressed turns the choice, with the game's beep for a move; the words written over.
void Smb1Engine::luigiTitleFrame(uint8_t mode) {
  const uint16_t pad = snes_.input[0];
  const uint16_t pressed = (uint16_t)(pad & ~luigiPrevPad_);
  luigiPrevPad_ = pad;
  luigiMenu_ = false;
  if (lostLevels_ || mode != 0 || !titleInput_) return;
  const uint16_t* vram = snes_.ppu.vram;
  const uint16_t row1 = 0x024a;
  const bool one = (vram[row1] & 0x3ff) == 0x01;
  const bool player = (vram[row1 + 2] & 0x3ff) == 0x19 || (vram[row1 + 1] & 0x3ff) == 0x19;
  luigiMenu_ = one && player && ram(0x0772) == 3;  // (OperMode_Task 3: the menu, not its setting up)
  if (!luigiMenu_) return;
  if (titleCursor_ == 0 && (pressed & 0x0300)) {  // (Left / Right)
    luigiPick_ = !luigiPick_;
    snes_.wram[0x1603] = 0x01;  // (the game's sound for a move)
  }
  luigiTitleText();
}
