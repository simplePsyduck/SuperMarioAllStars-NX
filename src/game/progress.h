// "Save Level Progress" (Cheats). The games' saves already have room for the
// progress within a world; the games throw it away. With this on, they keep it:
//
// * Super Mario Bros. 3: the save writes every map clear bit ($7E1D00-$1D7F
//   into the file, $00:925F), and loading reads them back ($20:8204), but then
//   ANDs them with a per-world mask ($20:8225: AND $8000,Y, data bank $21) that
//   keeps only some locks and fortresses: the levels come back unplayed. The
//   AND is skipped, so the map comes back as it was saved (Luigi's half is the
//   same copy, as the game does). Saves made before this was on hold the bits
//   too.
// * Super Mario Bros.: a game over's menu (Continue / Save & Continue / Save &
//   Quit, $03:A18D) puts the level and area back to 0 ($075C, $0760) before the
//   save, and the save itself ($00:8F03) writes level 0 and area 0 for a new
//   world ($8F6E, $8F73) or keeps the file's own for the same world ($8F5B,
//   $8F62). The pause menu's saves ($05:DF50, its cursor $0B77) do the same.
//   With this on, Save & Continue and Save & Quit (a game over's, or the pause
//   menu's) keep the level last played: the save writes it (its checksum is made from what it writes), the
//   game's own load starts the file there ($05:C965: the level and area from
//   the save), and Save & Continue goes on from it. A plain Continue still
//   starts the world again, as does a save from another world.
// * The Lost Levels saves and loads the level itself: nothing to do.
// * Super Mario Bros. 2 saves the level within the world too ($00:9152: the file's world and level,
//   from $04E8, unless the file is further on), and its start from a file takes both ($11:8122:
//   $0533 = world * 3 + level), but the All-Stars menu hands it level 0 ($00:A333, as Super Mario
//   Bros.'s): with this on, its files take the saved level, as The Lost Levels' do.
//
// * The power-up (all four): the games' saves don't keep it (SMB1 and The
//   Lost Levels start small, SMB3's map starts with no suit, SMB2 with two
//   hearts: the extra ones, $04C4, from the level's mushrooms). With this on,
//   each save also writes it into SRAM the games never use ($70:0600 on: per
//   All-Stars file and game, a 'P' record with the power-up and a copy of that
//   game's part of the file as the save left it), and a file started from the
//   All-Stars menu gets it back if its part is still that copy (a file erased,
//   copied over or saved with this off has none): SMB1 / LL $0756 (and $0754)
//   at the first frame of play, SMB3 $0747 / $0748 (Luigi's) when its map is up, and $0429 (the
//   suit drawn on the map), SMB2 $04C4 as the level starts ($14:E09D: the life meter filled from
//   it, so whichever character is picked has them). (SMB2's records follow the other three's:
//   $70:12C0 on, one per file.)
//
// Nothing is kept outside the battery save.
#pragma once
#include <cstdint>
#include <string>

class Snes;
class Smb1Engine;

class ProgressKeeper {
public:
  explicit ProgressKeeper(Snes& snes) : snes_(snes) {}

  bool enabled = false;  // (Cheats > Save Level Progress; off while achievements count)

  // The code addresses (bank $00/$80 mirrors are marked by SmasGame).
  static constexpr uint32_t kSmb3MaskAnd = 0x208225;      // AND $8000,Y (3 bytes)
  static constexpr uint32_t kSmb1GoLevel = 0x03a1a8;      // STZ $075C (3)
  static constexpr uint32_t kSmb1GoArea = 0x03a1b4;       // STZ $0760 (3)
  static constexpr uint32_t kSmb1SaveNewLevel = 0x008f6e; // LDA #$00 (2)
  static constexpr uint32_t kSmb1SaveNewArea = 0x008f73;  // LDA #$00 (2)
  static constexpr uint32_t kSmb1SaveKeepLevel = 0x008f5b;  // LDA $700010,X (4)
  static constexpr uint32_t kSmb1SaveKeepArea = 0x008f62;   // LDA $700010,X (4)
  // The All-Stars menu's file panel ($00:A30D): each file's furthest world and level, for its
  // world / level picker. Only The Lost Levels' files ((Y & 3) == 1) take the saved level
  // ($00:A333); Super Mario Bros.'s are at level 1. With this on, theirs take it too.
  static constexpr uint32_t kMenuFileLevel = 0x00a333;    // TYA (1)
  static constexpr uint32_t kMenuFileLevelLl = 0x00a33d;  // (the Lost Levels' way: LDA $00)
  // Super Mario Bros.'s start from a file ($05:C96F: LDA $7FFB02, the area): the area of the world
  // and level picked, worked out from them (the file's own is for its saved level, and the picker
  // can start an earlier one). Always (on or off): files saved with this on stay right with it off.
  static constexpr uint32_t kSmb1LoadArea = 0x05c96f;     // LDA $7FFB02 (4)
  // The save routines (their entries): Super Mario Bros., The Lost Levels, Super Mario Bros. 3.
  static constexpr uint32_t kSaveSmb1 = 0x008f03, kSaveLl = 0x0090ac, kSaveSmb3 = 0x00922d, kSaveSmb2 = 0x009152;
  // Super Mario Bros. 2: a level's start fills the life meter from the extra hearts ($14:E09D: LDY $04C4).
  static constexpr uint32_t kSmb2Hearts = 0x14e09d;
  static void markExec(Snes& snes);

  // `game` is a GameId (as int); `e` the SMB1 engine (Super Mario Bros.'s). True: handled (the
  // instruction is skipped, the CPU's registers set as it would have left them).
  bool onExec(uint32_t pc, int game, Smb1Engine* e);
  // Once per frame, after it.
  void frame(int game);
  void forget();  // a save state loaded

  // One-shot status for the player ("Level progress saved"), or empty.
  std::string takeMessage();

private:
  Snes& snes_;
  std::string message_;
  // Super Mario Bros.: the level being played (a game over's menu comes after it)
  int lastWorld_ = -1, lastLevel_ = 0;
  int lastGame_ = 0;
  bool keepLevel(int* level) const;  // the level a save now keeps (true: one past the first)
  // The power-up: taken as a save starts, written with it at the frame's end.
  int powerSaveGame_ = 0;  // (GameId as int: a save this frame; 0 none)
  uint8_t powerSave_[2] = {};
  int powerLoad_ = 0;      // (GameId: a game just started from the menu, its power-up still to give)
  int powerWait_ = 0;
  static int partOf(int game, int* size);
  static int recordAt(int file, int game);   // SRAM offset of a file's record for the game  // the game's part of a file: its offset in it, and size
  bool recordFor(int game, int* at) const;  // SRAM of the loaded file's record; true if it holds one
  void writePower(int game);
  bool applyPower(int game);
};
