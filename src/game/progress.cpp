// "Save Level Progress": see progress.h.
#include "game/progress.h"
#include <cstdio>
#include <cstring>
#include "core/snes.h"
#include "game/smas.h"
#include "game/smb1.h"

void ProgressKeeper::markExec(Snes& snes) {
  for (uint32_t b : {0x000000u, 0x800000u})
    for (uint32_t pc : {kSmb3MaskAnd, kSmb1GoLevel, kSmb1GoArea, kSmb1SaveNewLevel, kSmb1SaveNewArea,
                        kSmb1SaveKeepLevel, kSmb1SaveKeepArea, kMenuFileLevel, kSmb1LoadArea, kSaveSmb1, kSaveLl,
                        kSaveSmb3})
      snes.markExec(pc | b, true);
}

// Super Mario Bros.: the level a save keeps, if past the world's first. In a game over's menu, the
// level last played in this world; in the pause menu (Save & Continue / Save & Quit while
// playing), the level being played.
bool ProgressKeeper::keepLevel(int* level) const {
  const uint8_t* w = snes_.wram;
  if (w[0x077A] != 0) return false;  // (one player)
  int l = -1;
  if (w[0x0770] == 3 && lastWorld_ == w[0x075f]) l = lastLevel_;  // (OperMode: game over)
  else if (w[0x0770] == 1 && w[0x0776]) l = w[0x075c];             // (playing, paused: the pause menu)
  if (l <= 0 || l > 3) return false;
  *level = l;
  return true;
}

bool ProgressKeeper::onExec(uint32_t pc, int game, Smb1Engine* e) {
  pc &= 0x7fffff;
  CpuRegs& r = snes_.cpu.r;
  uint8_t* w = snes_.wram;
  const auto setA8 = [&](int v) {
    r.a = (uint16_t)((r.a & 0xff00) | (v & 0xff));
    r.n = (v & 0x80) != 0;
    r.z = (v & 0xff) == 0;
  };

  if (pc == kSmb1LoadArea) {
    // (on or off) the area for the world and level just set from the file and the picker
    if (game != (int)GameId::SMB1 || !e) return false;
    const int area = e->areaOf(w[0x075f], w[0x075c]);
    if (area < 0) return false;
    setA8(area);
    r.pc = (uint16_t)(r.pc + 4);
    return true;
  }
  if (!enabled) return false;
  if (pc == kSaveSmb1 || pc == kSaveLl || pc == kSaveSmb3) {
    // A save starts: the power-up as it is now (written with the save, at the frame's end).
    const int g = pc == kSaveSmb1 ? (int)GameId::SMB1 : pc == kSaveLl ? (int)GameId::SMBLL : (int)GameId::SMB3;
    if (g != game) return false;
    powerSaveGame_ = g;
    if (g == (int)GameId::SMB3) {
      powerSave_[0] = w[0x0747];
      powerSave_[1] = w[0x0748];
    } else {
      powerSave_[0] = w[0x0756];
      powerSave_[1] = w[0x0754];
    }
    return false;
  }
  if (pc == kMenuFileLevel) {
    // The file panel's setup, Super Mario Bros.'s files: their saved level, as The Lost Levels'.
    if ((r.y & 3) != 0) return false;
    r.pc = (uint16_t)kMenuFileLevelLl;
    return true;
  }

  if (game == (int)GameId::SMB3) {
    if (pc != kSmb3MaskAnd) return false;
    // (the clear bits just read back from the save: kept whole, not masked to the world's locks)
    if ((r.x & 0xff) == 0) {
      int n = 0;
      for (int i = 0; i < 0x40; i++)
        for (int k = 0; k < 8; k++) n += (snes_.peek(0x7ffb3c + i) >> k) & 1;
      if (n > 0) {
        char buf[64];
        snprintf(buf, sizeof buf, "Level progress restored (%d map spaces)", n);
        message_ = buf;
      }
    }
    r.pc = (uint16_t)(r.pc + 3);
    return true;
  }

  if (game != (int)GameId::SMB1 || !e) return false;
  int level = 0;
  if (!keepLevel(&level)) return false;
  const int world = w[0x075f];
  const int area = e->areaOf(world, level);
  if (area < 0) return false;

  if (pc == kSmb1GoLevel || pc == kSmb1GoArea) {
    // The game over menu's way on: Save & Continue / Save & Quit ($0F06 1 / 2) go on from the
    // level (a plain Continue, 0, from the world's start as usual).
    if (w[0x0f06] == 0) return false;
    if (pc == kSmb1GoLevel) w[0x075c] = (uint8_t)level;
    else w[0x0760] = (uint8_t)area;
    r.pc = (uint16_t)(r.pc + 3);
    return true;
  }
  if (pc == kSmb1SaveNewLevel || pc == kSmb1SaveNewArea) {
    // (the save of a world past the file's: its level and area, instead of 0 and 0)
    setA8(pc == kSmb1SaveNewLevel ? level : area);
    r.pc = (uint16_t)(r.pc + 2);
    if (pc == kSmb1SaveNewArea) {
      char buf[64];
      snprintf(buf, sizeof buf, "Level progress saved (World %d-%d)", world + 1, level + 1);
      message_ = buf;
    }
    return true;
  }
  if (pc == kSmb1SaveKeepLevel || pc == kSmb1SaveKeepArea) {
    // The save of the file's own world (or an earlier one: then the file keeps what it has). X has
    // moved one byte on per byte written: the world is the file's byte before the level.
    const int back = pc == kSmb1SaveKeepLevel ? 1 : 2;
    const uint8_t savedWorld = snes_.sram[(0x10 + r.x - back) & 0x1fff];
    if (savedWorld != world) return false;
    setA8(pc == kSmb1SaveKeepLevel ? level : area);
    r.pc = (uint16_t)(r.pc + 4);
    if (pc == kSmb1SaveKeepArea) {
      char buf[64];
      snprintf(buf, sizeof buf, "Level progress saved (World %d-%d)", world + 1, level + 1);
      message_ = buf;
    }
    return true;
  }
  return false;
}

// ---- the power-up records ($70:0600 on: never used by the games, checked over boot, the menu and
// all four games' play and saves). One per All-Stars file and game: 'P', the power-up (2 bytes), the
// game, then a copy of the game's part of the file.
namespace {
constexpr int kRecBase = 0x0600, kRecSize = 0x110, kRecHead = 4, kFileSize = 0x120;
int gameIndex(int game) {
  return game == (int)GameId::SMB1 ? 0 : game == (int)GameId::SMBLL ? 1 : game == (int)GameId::SMB3 ? 2 : -1;
}
}  // namespace

int ProgressKeeper::partOf(int game, int* size) {
  // ($00:A80C: per file, each game's offset in it: SMB1 +$00, LL +$09, SMB2 +$11, SMB3 +$1A)
  switch (gameIndex(game)) {
    case 0: *size = 9; return 0x00;
    case 1: *size = 8; return 0x09;
    case 2: *size = 0x106; return 0x1a;
  }
  *size = 0;
  return -1;
}

// The loaded file's record for this game ($70:0004: the file's offset for the game playing).
bool ProgressKeeper::recordFor(int game, int* at) const {
  int size = 0;
  const int part = partOf(game, &size);
  if (part < 0) return false;
  const int off = snes_.sram[4] | snes_.sram[5] << 8;
  const int file = off / kFileSize;
  if (file > 3 || off % kFileSize != part) return false;
  *at = kRecBase + (file * 3 + gameIndex(game)) * kRecSize;
  const uint8_t* r = snes_.sram + *at;
  return r[0] == 'P' && r[3] == gameIndex(game) && std::memcmp(r + kRecHead, snes_.sram + 0x10 + off, size) == 0;
}

void ProgressKeeper::writePower(int game) {
  int size = 0;
  const int part = partOf(game, &size);
  const int off = snes_.sram[4] | snes_.sram[5] << 8;
  const int file = off / kFileSize;
  if (part < 0 || file > 3 || off % kFileSize != part) return;
  uint8_t* r = snes_.sram + kRecBase + (file * 3 + gameIndex(game)) * kRecSize;
  r[0] = 'P';
  r[1] = powerSave_[0];
  r[2] = powerSave_[1];
  r[3] = (uint8_t)gameIndex(game);
  std::memcpy(r + kRecHead, snes_.sram + 0x10 + off, size);
  snes_.sramDirty = true;
}

bool ProgressKeeper::applyPower(int game) {
  int at = 0;
  if (!recordFor(game, &at)) return true;  // (nothing to give: done)
  uint8_t* w = snes_.wram;
  const uint8_t* r = snes_.sram + at;
  static const char* kSuits[] = {"Small", "Super", "Fire", "Raccoon", "Frog", "Tanooki", "Hammer"};
  if (game == (int)GameId::SMB3) {
    // (on the map: the suit the next level starts with, $23:C0DE)
    if (w[0x070a] != 0 || w[0x0100] != 0xc0) return false;  // (not yet: the map isn't up)
    w[0x0747] = r[1] < 7 ? r[1] : 0;
    w[0x0748] = r[2] < 7 ? r[2] : 0;
    w[0x0429] = w[0x0747 + (w[0x0726] & 1)];  // (the suit the map shows on its player)
    if (r[1] > 0 && r[1] < 7) message_ = std::string(kSuits[r[1]]) + " Mario restored";
  } else {
    if (w[0x0770] != 1) return false;  // (not yet playing)
    const uint8_t power = r[1] <= 2 ? r[1] : 0;
    w[0x0756] = power;
    w[0x0754] = power ? 0 : 1;  // (size: 0 big, 1 small)
    if (power) message_ = std::string(kSuits[power]) + " Mario restored";
  }
  return true;
}

void ProgressKeeper::frame(int game) {
  if (game != lastGame_) {
    lastWorld_ = -1;
    lastLevel_ = 0;
    // (a game started from the All-Stars menu: its file's power-up to give back)
    powerLoad_ = enabled && lastGame_ == (int)GameId::Menu && gameIndex(game) >= 0 ? game : 0;
    powerWait_ = 3600;
    lastGame_ = game;
  }
  if (powerSaveGame_) {
    if (powerSaveGame_ == game) writePower(game);
    powerSaveGame_ = 0;
  }
  if (powerLoad_ && (powerLoad_ != game || !enabled || --powerWait_ <= 0 || applyPower(game))) powerLoad_ = 0;
  const uint8_t* w = snes_.wram;
  if (game == (int)GameId::SMB1 && w[0x0770] == 1) {  // (OperMode: playing)
    lastWorld_ = w[0x075f];
    lastLevel_ = w[0x075c];
  }
}

void ProgressKeeper::forget() {
  const uint8_t* w = snes_.wram;
  // (a state loaded mid-game: the level in it)
  if (lastGame_ == (int)GameId::SMB1 && w[0x0770] == 1) {
    lastWorld_ = w[0x075f];
    lastLevel_ = w[0x075c];
  } else {
    lastWorld_ = -1;
    lastLevel_ = 0;
  }
}

std::string ProgressKeeper::takeMessage() {
  std::string m;
  m.swap(message_);
  return m;
}
