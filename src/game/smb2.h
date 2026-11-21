// Widescreen / HUD / enemy range / cheats / level select for Super Mario Bros. 2
// (All-Stars). Addresses come from docs/re/smb2.md.
#pragma once
#include <memory>
#include <cstdint>
#include <map>
#include <string>
#include "core/snes.h"
#include "game/spawncatchup.h"

class Smb2Engine {
public:
  explicit Smb2Engine(Snes& snes);

  void frameStart(int want, bool active, bool hudAnchor);
  void markExec();           // install the exec hooks (spawn sweep and catch-up)
  bool onExec(uint32_t pc);  // exec hook dispatcher
  int extL() const { return extL_; }
  int extR() const { return extR_; }
  bool gameplay() const { return gameplay_; }
  // The title and the story after it ($0100 = $40, $0217 = 0; the character
  // select and the card between levels have $0217 = 1, Bonus Chance $0789 =
  // 1): drawn as one 256 px picture, shown wide by SmasGame::pictureRemap.
  // Not once Start is pressed ($02E6 = $FF): the character select is wiped
  // in by scrolling the picture's parts, and the margins carried stripes of
  // it on.
  // (and Bonus Chance, its red frame carried on to the display's edges: in
  // BG mode 2 these are the title, its story and Bonus Chance. $0789 is
  // counted after its slots ($11:8E68) and $02E6 is left from the level, so
  // neither marks it.)
  // (and the warp card ("WARP World 7"), Bonus Chance's frame, in NMI mode
  // $C0: from a level)
  // (and the ending, mode $0775 = 9: the throne room, the contributors'
  // parade, Mario asleep and the cast: each one picture)
  // (The ending's throne room ($0217 = 1): framed by a gold block column
  // each side, 16 px; carried on as a picture's edge it came out in stripes:
  // repeated a tile column at a time instead, see SmasGame.)
  bool endingScreen() const { return pictureScreen() && ram(0x0775) == 9; }
  void paradeSprites(Ppu& ppu, int extR);
  void paradeStart(int extL);
  bool throneScreen() const { return pictureScreen() && ram(0x0775) == 9 && ram(0x0217) == 1; }
  bool pictureScreen() const {
    if (!gameplay_ && !endFade_ && ram(0x0100) == 0x40 && ram(0x0775) == 9) return true;
    return !gameplay_ && !endFade_ && ram(0x0217) == 0 &&
           ((ram(0x0100) == 0x40 && (ram(0x0775) == 2 || (ram(0x0789) == 0 && ram(0x02e6) == 0))) ||
            (ram(0x0100) == 0xc0 && ram(0x0775) == 2));
  }
  // The character select ($0217 = 1 in BG mode 2; the card has mode 1): its
  // frame's diamond lattice is carried on over the margins (SmasGame).
  // (NMI mode $40 after the title, $C0 after a level or a death: either.)
  bool selectScreen() const {
    return !gameplay_ && !endFade_ && ram(0x0217) == 1 && ram(0x0775) == 2;
  }
  // The card between levels ("WORLD 1-1", BG mode 1): its scattered motifs
  // are carried on over the margins (SmasGame).
  bool cardScreen() const {
    return !gameplay_ && !endFade_ && ram(0x0217) == 1 && ram(0x0775) == 1;
  }

  int fetchWideTile(Ppu& ppu, int layer, int tx, int ty, int screenX, uint16_t* entry);
  void beginLine(Ppu& ppu, int row);
  void placeHudSprites(Ppu& ppu);
  // Development check: renders the visible 4:3 area of `layer` from RAM and
  // counts tiles that differ from VRAM. Returns "compared/mismatched".
  std::string verify(int layer);

  enum {
    kCheatLives = 1 << 0,
    kCheatHealth = 1 << 1,
    kCheatStar = 1 << 2,
    kCheatHearts = 1 << 3,
    kCheatJump = 1 << 4,
    kCheatFloat = 1 << 5,
  };
  void applyCheats(uint32_t flags);

  // level select (world 0-6, level within world 0-2)
  bool atTitle() const;
  bool inGame() const;
  void requestWarp(int world, int level, int character = -1);  // (character: $008F's, -1 the highlighted one)
  // Forget per-session state (pending warp, view adjustments): state load, reset.
  void reset() {
    warpStage_ = warpTimer_ = 0;
    input_ = 0;
    bgAdj_[1] = bgAdj_[2] = 0;
    forgetBands();
    subspace_ = false;
    verticalArea_ = false;
    roomJar_ = false;
    oneRoomH_ = false;
    wallTile_[0] = wallTile_[1] = -1;
    jar_ = false;
    latchInit_ = false;
    sweeping_ = swept_ = false;
    spawn_.reset();
  }
  bool overridesInput() const { return warpStage_ != 0; }
  uint16_t inputOverride() const { return input_; }
  std::string takeMessage();
  std::string debug() const;
  // Widescreen state kept in save states (see SmasGame::saveExt).
  void saveExt(std::vector<uint8_t>& out) const;
  bool loadExt(const uint8_t* p, size_t n);
  // A state without that data: take it as saved with the game's own range.
  void assumeNarrowState() { spawn_.inactive(); }

private:
  Snes& snes_;
  int extL_ = 0, extR_ = 0;
  int ext_ = 0;  // requested margin per side (the view window may shift it)
  // BG2/BG3 on the current line: scroll adjustment while the view is held,
  // absolute scroll (for the RAM renderer), and per band kind the game's
  // rounding (scroll - floor(speed x camera)) kept from before the hold.
  int16_t bgAdj_[3] = {};
  int layerAbs_[3] = {};
  int lineHofs_[3] = {-1, -1, -1};
  int shift_ = 0;
  int holdDev_[3][8] = {};
  bool holdSeen_[3][8] = {};
  // Per band (in order down the screen) the kind it matched last frame.
  int8_t bandKind_[3][8];
  int bandIdx_[3] = {};
  // Per band: its adjustment last frame and what is kept from an earlier
  // kind (a band's kind can only be told once the camera has moved).
  int bandAdj_[3][8] = {};
  int bandRow_[3][8] = {}, prevBandRow_[3][8] = {};  // first line of each band, this frame / last
  int bandOff_[3][8] = {};
  int prevShift_ = 0;
  bool dark_ = false;
  void forgetBands() {
    for (auto& s : holdSeen_)
      for (bool& b : s) b = false;
    for (auto& s : bandKind_)
      for (int8_t& k : s) k = -1;
    for (auto& s : bandOff_)
      for (int& o : s) o = 0;
  }
  void layerLine(int l, int h, int band);
  bool subspace_ = false;  // in subspace (held until the screen goes black)
  bool verticalArea_ = false;  // in a vertical area (likewise)
  bool roomJar_ = false;       // ... one screen tall: a room, shown as a jar's (likewise)
  int wallTile_[2] = {-1, -1};  // ... walled both sides all the way up: the walls' tiles carried on (likewise)
  void findWalledVertical();
  int openBg_ = -1;     // a vertical area's commonest tile (see findWalledVertical)
  int16_t nextL_[512] = {}, nextR_[512] = {};  // ... the tile most often left / right of each (+$100 flagged; -1: none)
  uint16_t keyEntry(int key, int qx, int qy) const;
  bool edgeTile_[512] = {};  // ... the tiles seen in its edge columns
  bool innerTile_[512] = {};  // ... its walls' inner tiles (wall both sides)
  bool edgeOnly_[512] = {};   // ... tiles seen only in its edge columns
  int16_t rawL_[512] = {}, rawR_[512] = {};  // ... the tile most often left / right of each, any (-1: none)
  int openFill_ = -1;        // ... the commonest of those
  bool openVertical() const;
  int openVerticalTile(int tx, int ty, int screenX, uint16_t* entry, bool oneScreen, int* keyOut = nullptr) const;
  int openVerticalTileRaw(int tx, int ty, int screenX, uint16_t* entry, bool oneScreen, int* keyOut) const;
  // (openVerticalTile's last answer: the renderer asks again pixel by pixel after a blank, and within a tile on one
  // line the answer only changes with screenX's 16 px column (and the margin's last 16 px): see openVerticalTile)
  struct OvtMemo {
    uint64_t stamp = ~0ull;
    int tx = 0, ty = 0, col = 0, vofs = 0, extL = 0, extR = 0;
    bool last = false, oneScreen = false;
    int r = 0;
    uint16_t e = 0;
  };
  mutable OvtMemo ovtMemo_;
  bool oneRoomH_ = false;  // a horizontal area one screen wide (latched as verticalArea_)
  int bg1Vofs_ = 0;        // BG1's vertical scroll this frame (line 0)
 public:
  bool dimWrap_ = true;    // dim the margins of areas that wrap round (option)
 private:
  uint16_t metaEntry(int mid, int qx, int qy) const;
  bool jar_ = false;           // inside a jar (likewise): shown wide, the scenery around it
  bool latchInit_ = false;     // the three above taken from the game since the last reset
  // A jar's margins: no BG1 (the jar stands alone), BG2/BG3 the screen the
  // game shows, repeated where its edges join up, else mirrored about them;
  // only what is joined to the screen's edge (through non-blank tiles), not
  // things standing alone (2-1's moon).
  bool jarMirror_[3] = {};
  uint16_t jarBlank_[3] = {};
  int jarRow0_[3] = {};
  bool jarKeep_[3][2][28][32] = {};  // [layer][0: joined to the left edge, 1: the right][row][col]
  void jarEdges(const Ppu& ppu);
  void jarEdgesWork(const Ppu& ppu);
  // (jarEdges' inputs when it last worked: the two layers' scroll, map place and size, map words, and whether the
  // area is open or walled; the same again, its results stand)
  struct JarInputs {
    uint16_t hofs[2], vofs[2], mapBase[2];
    uint8_t mapSize[2];
    bool open;
    uint16_t map[2][4096];
  };
  std::unique_ptr<JarInputs> jarIn_;
  bool jarInValid_ = false;
  int jarTile(const Ppu& ppu, int layer, int tx, int ty, uint16_t* entry) const;
  bool openMirror_[3] = {};  // an open vertical area's layers mirrored at the edges (else repeated)
  uint16_t openBlank_[3] = {};
  bool openKeep_[3][2][64][32] = {};  // ... joined to the left / right edge, over the whole map
  int openLayerTile(const Ppu& ppu, int layer, int tx, int ty, uint16_t* entry) const;
  int16_t paradeX_[128] = {};
  bool paradeSet_[128] = {};
  int paradeExtR_ = 0;
  bool hudAnchor_ = true;
  bool trueX_ = false;  // sprite positions set in the PPU (see placeHudSprites)
  // Enemy spawning with a widened range (see spawncatchup.h).
  int want_ = 0;
  bool active_ = false, sweeping_ = false, swept_ = false;
  bool wideRange_ = false;  // the widened spawn/despawn range patched in this frame
  bool selfH_[512] = {};    // tiles seen beside themselves in a row (see openVerticalTile)
  int16_t beyondL_[512] = {}, beyondR_[512] = {};  // what is beside a tile each way, seen twice (or itself); -1
  int16_t surfaceOf_[512] = {};  // what tops a tile where it is a surface (seen twice); -1
  bool noneL_[512] = {}, noneR_[512] = {};  // nothing ever beside a tile that way
  bool floorUnder_[2][160] = {};  // [side][row]: an end piece under a wall carried on (carried on too)
  bool runOpen_[2][160] = {};  // [side][row]: the edge column's run there is a frame, open beyond
  int runEnd_[2][160] = {};    // ... the run's last row
  // The edges (where a vertical area wraps round) the player can get to
  // within the rows on screen (1 left, 2 right): those margins dimmed,
  // faded in and out (0 .. 96 each side).
  int wrapSides_ = 0;
  int dimAmt_[2] = {};
  int playerReachesEdge() const;
  static bool passable(int key);
  SpawnCatchUp spawn_;
  void catchUpSpawn();
  bool generatorAt43() const;
  int generatorCol43(int cam) const;
  void generators(int cam);
  void restoreGenerator();
  void makeRoom();
  int slotToFree(int idx, int col) const;
  bool canMakeRoom(int idx, int col) const;
  // Level enemies turned away for want of a slot, tried again (see spawnRefused).
  struct Retry {
    int idx, col;  // enemy list offset, level column
  };
  static constexpr int kRetryMax = 8;
  Retry retry_[kRetryMax] = {};
  int retryN_ = 0;
  void spawnRefused();
  bool retryColumn(int cam, int* col);
  bool warpSeenSelect_ = false;
  void viewSplit(int cam, int* l, int* r) const;
  bool gameplay_ = false;
  bool endFade_ = false;  // play just ended (level end): the view kept until the screen is black
  uint8_t shownArea_ = 0;  // the area ($0534) last shown (see beginLine)
  int lastBright_ = 0;
  bool maskArea_ = false;
  int endFrames_ = 0;
  bool horizontal_ = true;
  int cam16_ = 0;
  std::map<uint32_t, uint8_t> original_;
  int warpStage_ = 0, warpTimer_ = 0, warpWorld_ = 0, warpLevel_ = 0, warpChar_ = -1;
  uint16_t input_ = 0;
  std::string message_;

  uint8_t ram(uint16_t a) const { return snes_.wram[a]; }
  void patch(uint32_t adr, uint8_t v);
  void unpatchAll();
  void updateWarp();
  // RAM renderers: return false if the position is outside the data.
  bool bg1Entry(int x, int y, uint16_t* e) const;
  bool bgLayerEntry(int layer, int x, int y, uint16_t* e) const;
  int layerScrollX(int layer) const;
  // Scenery outside a parallax layer's drawn pages (see bgLayerEntry): the
  // loop it repeats with (px, 0 = none), else mirrored when the first page
  // begins inside a shape, else the first page repeats.
  int layerLoop_[3] = {}, layerEnd_[3] = {};
  bool mirrorStart_[3] = {};
  int layerBase(int layer) const;
  int layerPeriod(int layer, int* pages) const;
  bool startCut(int layer) const;
  void layerEdges();
};
