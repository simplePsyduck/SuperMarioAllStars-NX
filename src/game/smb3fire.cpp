// More Active Fireballs (Cheats; Super Mario Bros. 3): up to 15 of the
// player's fireballs at once instead of 2.
//
// The player's projectiles have two slots ($1CE1,X: 0 none, 1 fireball, 2
// hammer, 3 and up a puff). The projectile step ($27:B42C) runs slot 1 (JSR
// $B436), then falls into the same code for slot 0, whose RTS returns to its
// caller ($27:B2B7). There, each fireball flying in a slot moves into a pool
// kept here, which frees the slot for the next throw; then each fireball in
// the pool takes its turn in slot 1: its variables put in, $B436 called as the
// game calls it (with $9B, the slot being run, 1), returning to $27:B2B7, its
// variables taken out. The game's own code does the rest: the bounces, hitting
// enemies and blocks, the puff, the drawing (its sprites found free each time,
// $27:D728). Hammers stay in the game's slots.
//
// What a fireball is, found by watching the step run for slot 0 and slot 1
// (smastool, 2026-11): the bytes below, slot 1's one on from slot 0's. Others
// it touches are the same for both or worked out in each run ($C6,X: its
// sprites' offset, found afresh; $0783,X is the random number generator).
// In co-op each fireball keeps its thrower (projOwner_, see coopProjBefore).
#include <cstring>
#include "core/snes.h"
#include "game/smb3.h"

namespace {
const uint16_t kFbAddr0[Smb3Engine::kFbVars] = {
    0x1ce1,  // state (0 none, 1 fireball, 2 hammer, 3+ puff)
    0x1ce3, 0x1ce5, 0x1ce7, 0x1ce9, 0x1ceb, 0x1ced,  // position, speeds, timers
    0x0247,  // X (low)
    0x05fa,  // X (high)
};
inline uint16_t fbAddr(int slot, int i) { return (uint16_t)(kFbAddr0[i] + slot); }
constexpr uint32_t kProjSlot = 0x27b436;  // the step for slot X
constexpr uint32_t kProjBack = 0x27b2b7;  // where slot 0's run returns
}  // namespace

void Smb3Engine::fireClear() {
  for (FbBall& b : fb_) b.live = b.done = false;
  fbCur_ = -1;
}

int Smb3Engine::fireballsInFlight() const {
  int n = 0;
  for (const FbBall& b : fb_) n += b.live;
  for (int s = 0; s < 2; s++) n += snes_.wram[fbAddr(s, 0)] == 1;
  return n;
}

bool Smb3Engine::fireExec() {
  CpuRegs& r = snes_.cpu.r;
  uint8_t* w = snes_.wram;
  if (fbCur_ >= 0) {
    // A pool fireball's turn has ended: back to the pool, slot 1 as it was.
    FbBall& b = fb_[fbCur_];
    for (int i = 0; i < kFbVars; i++) {
      const uint16_t a = fbAddr(1, i);
      b.v[i] = w[a];
      w[a] = fbHold_[i];
    }
    b.owner = projOwner_[1];
    projOwner_[1] = fbHold_[kFbVars];
    w[0x009b] = fbHold_[kFbVars + 1];
    b.live = b.v[0] != 0;
    if (!b.live) b.owner = 0;
    b.done = true;
    fbCur_ = -1;
  } else {
    // The game's own two slots have just been run.
    // (Another area, or out of play: the pool's fireballs went with it.)
    if (areaStamp_ != fbArea_ || !gameplay_) fireClear();
    fbArea_ = areaStamp_;
    if (!fireCheat_) {
      fireClear();
      return false;
    }
    for (FbBall& b : fb_) b.done = false;
    // Fireballs flying in the slots into the pool (run already this frame): the slots free.
    for (int s = 0; s < 2; s++) {
      if (w[fbAddr(s, 0)] != 1) continue;
      FbBall* free = nullptr;
      for (FbBall& b : fb_)
        if (!b.live) {
          free = &b;
          break;
        }
      if (!free) break;  // (15 in all: the slots keep theirs)
      for (int i = 0; i < kFbVars; i++) free->v[i] = w[fbAddr(s, i)];
      free->owner = projOwner_[s];
      free->live = free->done = true;
      w[fbAddr(s, 0)] = 0;
      projOwner_[s] = 0;
    }
  }
  // The next pool fireball's turn: into slot 1, and the step called for it (JSR: back here).
  for (int k = 0; k < kFbPool; k++) {
    FbBall& b = fb_[k];
    if (!b.live || b.done) continue;
    for (int i = 0; i < kFbVars; i++) {
      const uint16_t a = fbAddr(1, i);
      fbHold_[i] = w[a];
      w[a] = b.v[i];
    }
    fbHold_[kFbVars] = projOwner_[1];
    fbHold_[kFbVars + 1] = w[0x009b];
    projOwner_[1] = b.owner;
    w[0x009b] = 1;
    fbCur_ = k;
    auto push = [&](uint8_t v) {
      w[r.sp & 0x1fff] = v;
      r.sp = r.e ? (uint16_t)(0x100 | ((r.sp - 1) & 0xff)) : (uint16_t)(r.sp - 1);
    };
    const uint16_t ret = (uint16_t)((kProjBack & 0xffff) - 1);
    push(ret >> 8);
    push(ret & 0xff);
    r.x = (uint16_t)((r.xf ? 0 : (r.x & 0xff00)) | 1);
    r.pc = (uint16_t)(kProjSlot & 0xffff);
    return true;
  }
  return false;
}

void Smb3Engine::fireSave(std::vector<uint8_t>& out) const {
  int n = 0;
  for (const FbBall& b : fb_) n += b.live;
  if (!n) return;
  out.push_back('F');
  out.push_back((uint8_t)kFbVars);
  out.push_back((uint8_t)n);
  for (int k = 0; k < kFbPool; k++)
    if (fb_[k].live) {
      out.push_back((uint8_t)k);
      out.push_back(fb_[k].owner);
      out.insert(out.end(), fb_[k].v, fb_[k].v + kFbVars);
    }
}

void Smb3Engine::fireLoad(const uint8_t*& p, size_t n) {
  fireClear();
  fbArea_ = areaStamp_;
  if (!p || n < 3 || p[0] != 'F' || p[1] != kFbVars) return;
  const int count = p[2];
  if (n < 3 + (size_t)count * (2 + kFbVars)) return;
  p += 3;
  for (int i = 0; i < count; i++) {
    const int k = p[0];
    if (k < kFbPool) {
      fb_[k].owner = p[1];
      std::memcpy(fb_[k].v, p + 2, kFbVars);
      fb_[k].live = true;
    }
    p += 2 + kFbVars;
  }
}
