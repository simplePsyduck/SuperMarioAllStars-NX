// More Active Fireballs (Cheats; Super Mario Bros. and The Lost Levels): up to
// 15 of Mario's fireballs at once instead of 2.
//
// The game has two fireball slots (Fireball_State $33,X; their SprObjects are
// 11 and 12). ProcFireball_Bubble throws into slot FireballCounter & 1 when it
// is free, then runs FireballObjCore for slot 0 and slot 1 (LDX #$00; JSR;
// LDX #$01; JSR). Here, just after those two calls, each fireball in flight in
// a slot moves into a pool kept here, which frees the slot for the next throw.
// Then every fireball in the pool takes its turn in slot 1: its variables put
// in, FireballObjCore run (called as the game calls it, returning to this same
// place), its variables taken back out. The game's own code does everything:
// the flight and bounces, hitting enemies, the explosion, the drawing (each
// pool fireball with its own sprite, $02-$0E, put in its sprite offset $0B57).
//
// What a fireball is, found by watching FireballObjCore run for slot 0 and slot
// 1 (smastool, 2026-11): the bytes below, slot 1 one on from slot 0 (its
// bounding box four on). The rest it touches is the same for both (relative
// position, offscreen bits, temporaries), worked out again in each run.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include "core/snes.h"
#include "game/smb1.h"

namespace {
// Slot 0's address of each; slot 1's is +1 (the last four, its bounding box, +4).
const uint16_t kFbAddr0[Smb1Engine::kFbVars] = {
    0x0033,  // Fireball_State (0 none, 1 flying, 2 just thrown, bit 7 exploding)
    0x0068,  // X speed
    0x0083,  // page
    0x00ab,  // Y speed
    0x00c6,  // Y high byte
    0x020d,  // moving direction
    0x0224,  // X
    0x0242,  // Y
    0x040c, 0x0427, 0x0447,  // movement forces / fractions
    0x049a,  // bounding box size
    0x0fc8, 0x0fc9, 0x0fca, 0x0fcb,  // bounding box
};
inline uint16_t fbAddr(int slot, int i) { return (uint16_t)(kFbAddr0[i] + slot * (i >= 12 ? 4 : 1)); }
constexpr uint16_t kSprOffset1 = 0x0b57;  // slot 1's sprite (OAM byte offset)
constexpr int kFirstSprite = 0x02;        // sprites $02-$0E: never used by the game (checked over its levels)
}  // namespace

void Smb1Engine::fireInit(uint8_t bankLo, uint8_t bankHi) {
  // ProcFireball_Bubble: LDA PlayerStatus; CMP #$02; BCC; LDA A_B_Buttons; AND #B_Button; BEQ;
  // AND PreviousA_B_Buttons; BNE; LDA FireballCounter; AND #$01; TAX; LDA Fireball_State,X; BNE
  static const uint16_t kSigProcFire[] = {0xAD, 0x56, 0x07, 0xC9, 0x02, 0x90, 0x100, 0xA5, 0x0A, 0x29, 0x40,
                                          0xF0, 0x100, 0x25, 0x0D, 0xD0, 0x100, 0xAD, 0xCE, 0x06, 0x29,
                                          0x01, 0xAA, 0xB5, 0x33, 0xD0};
  // (ForceInjury: LDX $0756; BEQ; STA $0756; LDA #$08; STA $07AE; ... LDA #$0A at +$16; the exit,
  // LDX $9E; RTS, at +$27)
  static const uint16_t kSigInjury[] = {0xAE, 0x56, 0x07, 0xF0, 0x100, 0x8D, 0x56, 0x07, 0xA9, 0x08, 0x8D, 0xAE, 0x07};
  injStore_ = injShrink_ = injExit_ = 0;
  if (const uint32_t j = findSig(kSigInjury, sizeof(kSigInjury) / 2, bankLo, bankHi))
    if (snes_.peek(j + 0x16) == 0xA9 && snes_.peek(j + 0x17) == 0x0A && snes_.peek(j + 0x27) == 0xA6 &&
        snes_.peek(j + 0x28) == 0x9E && snes_.peek(j + 0x29) == 0x60) {
      injStore_ = j + 5;
      injShrink_ = j + 0x16;
      injExit_ = j + 0x27;
    }
  carryInit(bankLo, bankHi);  // (Pick Up & Carry Koopa Shells: smb1carry.cpp)
  fbCore_ = fbRet_ = 0;
  const uint32_t f = findSig(kSigProcFire, sizeof(kSigProcFire) / 2, bankLo, bankHi);
  if (!f) return;
  // (+$41: LDX #$00; JSR FireballObjCore; LDX #$01; JSR FireballObjCore, the same routine)
  const uint32_t c = f + 0x41;
  if (snes_.peek(c) != 0xA2 || snes_.peek(c + 1) != 0x00 || snes_.peek(c + 2) != 0x20 || snes_.peek(c + 5) != 0xA2 ||
      snes_.peek(c + 6) != 0x01 || snes_.peek(c + 7) != 0x20 || snes_.peek(c + 3) != snes_.peek(c + 8) ||
      snes_.peek(c + 4) != snes_.peek(c + 9))
    return;
  fbCore_ = (f & 0xff0000) | snes_.peek(c + 3) | snes_.peek(c + 4) << 8;
  fbRet_ = c + 10;
}

// ---- Fire Mario goes to large Mario when hit (Cheats). ForceInjury (SMB1 $03:DF52, LL $0D:DE8A):
// LDX PlayerStatus; BEQ KillPlayer; STA PlayerStatus (A = 0: small); the injury timer, the sound, the
// palette (JSL GetPlayerColors); then LDA #$0A; LDY #$01; STA GameEngineSubroutine; STY Player_State;
// the timers halted (TimerControl $FF): the shrinking, the game standing still. With this on, a fiery
// player is set to 1 (big) instead, and instead of the shrinking runs the fire flower's routine ($0C,
// PlayerFireFlower: the palette flashing while everything stands still, as when a power-up is taken),
// his own state kept: big again, blinking for the injury timer as after any hit.
bool Smb1Engine::injExec(uint32_t pc) {
  CpuRegs& r = snes_.cpu.r;
  if (pc == injStore_) {
    injFire_ = (cheats_ & kCheatFireToBig) && ram(0x0756) == 2;
    if (injFire_) r.a = (uint16_t)((r.a & 0xff00) | 1);
    return false;
  }
  if (pc == injShrink_ && injFire_) {
    injFire_ = false;
    r.a = (uint16_t)((r.a & 0xff00) | 0x0c);           // (PlayerFireFlower)
    r.y = (uint16_t)((r.y & 0xff00) | ram(0x0028));    // ([Player_State] as it is)
    r.pc = (uint16_t)(injShrink_ + 4);                 // (on at STA $0F: then the timers halted)
    return true;
  }
  return false;
}

void Smb1Engine::fireClear() {
  for (FbBall& b : fb_) b.live = b.done = false;
  fbCur_ = -1;
}

int Smb1Engine::fireballsInFlight() const {
  int n = 0;
  for (const FbBall& b : fb_) n += b.live;
  for (int s = 0; s < 2; s++) n += snes_.wram[fbAddr(s, 0)] != 0;
  return n;
}

bool Smb1Engine::fireExec() {
  CpuRegs& r = snes_.cpu.r;
  uint8_t* w = snes_.wram;
  if (fbCur_ >= 0) {
    // A pool fireball's turn has ended (FireballObjCore returned here): back to the pool, and slot
    // 1 as it was.
    FbBall& b = fb_[fbCur_];
    for (int i = 0; i < kFbVars; i++) {
      const uint16_t a = fbAddr(1, i);
      b.v[i] = w[a];
      w[a] = fbHold_[i];
    }
    w[kSprOffset1] = fbHold_[kFbVars];
    b.live = b.v[0] != 0;
    b.done = true;
    fbCur_ = -1;
  } else {
    // The game's own two fireballs have just been run.
    // (Another area: its fireballs went with it, as the game's own are cleared.)
    const uint32_t area = (uint32_t)w[0x075f] << 16 | w[0x0750] << 8 | w[0x0760];
    if (area != fbArea_) fireClear();
    fbArea_ = area;
    if (!(cheats_ & kCheatFireballs)) {
      fireClear();
      return false;
    }
    for (FbBall& b : fb_) b.done = false;
    // Fireballs flying in the game's slots (state 1; not one just thrown, nor bursting) into the
    // pool: the slots free for the next throws. (Run already this frame: done.)
    for (int s = 0; s < 2; s++) {
      if (w[fbAddr(s, 0)] != 1) continue;
      FbBall* free = nullptr;
      for (FbBall& b : fb_)
        if (!b.live) {
          free = &b;
          break;
        }
      if (!free) break;  // (the pool is full: 15 in all, the slots keep theirs)
      for (int i = 0; i < kFbVars; i++) free->v[i] = w[fbAddr(s, i)];
      free->live = free->done = true;
      w[fbAddr(s, 0)] = 0;
    }
  }
  // The next pool fireball's turn: into slot 1, with its own sprite, and FireballObjCore called
  // as the game calls it (JSR: back here, to this instruction).
  for (int k = 0; k < kFbPool; k++) {
    FbBall& b = fb_[k];
    if (!b.live || b.done) continue;
    for (int i = 0; i < kFbVars; i++) {
      const uint16_t a = fbAddr(1, i);
      fbHold_[i] = w[a];
      w[a] = b.v[i];
    }
    fbHold_[kFbVars] = w[kSprOffset1];
    w[kSprOffset1] = (uint8_t)((kFirstSprite + k) * 4);
    fbCur_ = k;
    auto push = [&](uint8_t v) {
      w[r.sp & 0x1fff] = v;
      r.sp = r.e ? (uint16_t)(0x100 | ((r.sp - 1) & 0xff)) : (uint16_t)(r.sp - 1);
    };
    const uint16_t ret = (uint16_t)(fbRet_ - 1);
    push(ret >> 8);
    push(ret & 0xff);
    r.x = (uint16_t)((r.xf ? 0 : (r.x & 0xff00)) | 1);
    r.pc = (uint16_t)fbCore_;
    return true;
  }
  return false;
}

void Smb1Engine::fireSave(std::vector<uint8_t>& out) const {
  int n = 0;
  for (const FbBall& b : fb_) n += b.live;
  if (!n) return;
  out.push_back('F');
  out.push_back((uint8_t)kFbVars);
  out.push_back((uint8_t)n);
  for (int k = 0; k < kFbPool; k++)
    if (fb_[k].live) {
      out.push_back((uint8_t)k);
      out.insert(out.end(), fb_[k].v, fb_[k].v + kFbVars);
    }
}

void Smb1Engine::fireLoad(const uint8_t*& p, size_t& n) {
  fireClear();
  fbArea_ = (uint32_t)snes_.wram[0x075f] << 16 | snes_.wram[0x0750] << 8 | snes_.wram[0x0760];
  if (n < 3 || p[0] != 'F' || p[1] != kFbVars) return;
  const int count = p[2];
  if (n < 3 + (size_t)count * (1 + kFbVars)) return;
  p += 3;
  n -= 3;
  for (int i = 0; i < count; i++) {
    const int k = p[0];
    if (k < kFbPool) {
      std::memcpy(fb_[k].v, p + 1, kFbVars);
      fb_[k].live = true;
    }
    p += 1 + kFbVars;
    n -= 1 + kFbVars;
  }
}

std::string Smb1Engine::fireDebug() const {
  std::string out;
  char buf[48];
  for (int s = 0; s < 2; s++) {
    snprintf(buf, sizeof buf, "s%d:%02X/%02X/%02X ", s, snes_.wram[fbAddr(s, 0)], snes_.wram[fbAddr(s, 4)], snes_.wram[fbAddr(s, 7)]);
    out += buf;
  }
  for (int k = 0; k < kFbPool; k++)
    if (fb_[k].live) {
      snprintf(buf, sizeof buf, "p%d:%02X/%02X/%02X ", k, fb_[k].v[0], fb_[k].v[4], fb_[k].v[7]);
      out += buf;
    }
  return out;
}
