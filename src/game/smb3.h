// Widescreen / HUD / enemy range / cheats / level select for Super Mario Bros. 3
// (All-Stars). Addresses come from docs/re/smb3.md.
#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include "core/snes.h"
#include "game/spawncatchup.h"

// Scroll adjustment keeping a parallax layer consistent with a shifted view
// (shared with SMB2).
int parallaxAdjust(double speed, int cam, int shift);

class Smb3Engine {
public:
  explicit Smb3Engine(Snes& snes);
  bool dimWrap_ = true;  // dim margins the player can't get to (option; see beginLine)

  void frameStart(int want, bool active, bool hudAnchor);
  void markExec();              // install the exec hooks (level-load object sweep)
  bool onExec(uint32_t pc);     // exec hook dispatcher
  void onWrite(uint32_t adr, uint8_t v);  // write hook (sprite buffer)
  int extL() const { return extL_; }
  int extR() const { return extR_; }
  bool gameplay() const { return gameplay_; }
  bool inPlayerUpdate() const { return inPlayer_; }  // (debugging: inside the player's update, $23:BFA7)
  // ---- two players at once (an experimental test: see smb3coop.cpp)
  void setCoop(bool on);
  void forgetRoom() { pieceLatched_ = pieceOpenDone_ = false; }  // (a state loaded: the room's walls are decided afresh)
  bool coopOn() const { return coop_; }
  bool coopMidTurn() const;  // (Luigi's variables in: see smb3coop.cpp)
  void coopCheats(uint32_t f);  // (the star, flight and P cheats for Luigi too)
  void coopPoke(uint32_t adr, uint8_t v);  // (testing: one of Luigi's variables)
  // The title's fourth choice, CO-OP GAME (see smb3coop.cpp): drawn after each frame;
  // the pick once Start takes it (1 co-op, 0 not; -1: none since).
  void titleFrame();
  void hudFrame();  // (the status bar in co-op: Mario's M and Luigi's L badges; at line 0)
  int takeTitlePick() {
    int p = titlePick_;
    titlePick_ = -1;
    return p;
  }
  // The title screen ($0100 = $20; the ending uses it too, with $070A = 2):
  // shown wide by SmasGame::pictureRemap.
  bool pictureScreen() const { return ram(0x0100) == 0x20 && ram(0x070a) != 2; }
  // The spade game (tileset $0F): a framed picture on black whose slider rows
  // scroll by raster splits; shown wide the same way, its black carried on.
  bool spadeScreen() const { return ram(0x070a) == 0x0f && ram(0x0100) == 0xc0; }
  // The battle game (tileset $12): one screen that wraps round, the players
  // leaving one side coming back in at the other.
  bool battleScreen() const { return ram(0x070a) == 0x12 && ram(0x0100) == 0xc0; }
  bool mapScreen() const { return ram(0x070a) == 0 && ram(0x0100) == 0xc0; }  // the world map (the title: $0100 = $20)
  bool mapLeaving() const { return ram(0x0100) == 0x00; }  // ... a level being entered from it ("MARIO START!")

  void beginLine(Ppu& ppu, int row);
  int fetchWideTile(Ppu& ppu, int layer, int tx, int ty, int screenX, uint16_t* entry);

  // cheats
  enum {
    kCheatLives = 1 << 0,
    kCheatStar = 1 << 1,
    kCheatFlight = 1 << 2,
    kCheatPMeter = 1 << 3,
    kCheatTime = 1 << 4,
    kCheatRaccoon = 1 << 5,
    kCheatItems = 1 << 6,
    kCheatNoDamage = 1 << 7,
    kCheatFireballs = 1 << 8,  // More Active Fireballs: up to 15 at once (see smb3fire.cpp; cheat id 32)
  };
  void applyCheats(uint32_t flags);

  // level select: level 0 = world map, 1..10 = numbered levels, 11..13 = fortresses
  bool atTitle() const;
  void requestWarp(int world, int level);
  // Forget per-session state (pending warp, view glide, layer-speed samples):
  // state load, reset. Learned layer speeds per tileset are kept.
  void reset() {
    fireClear();
    warpStage_ = warpTimer_ = 0;
    input_ = 0;
    viewInit_ = false;
    levelShown_ = false;
    irisR_ = -1;
    holdInit_ = false;
    adoptPending_ = false;
    for (auto& h : held_) h = kFree;
    forgetParked();
    forgetHeldControls();
    drawing_ = -1;
    for (bool& b : oamXSet_) b = false;
    autoL_ = -1;
    easing_ = roomGlide_ = false;
    sweeping_ = swept_ = false;
    spawn_.reset();
    speedPrevOk_ = false;
    speedTileset_ = -1;
    sumCam_[1] = sumCam_[2] = sumLayer_[1] = sumLayer_[2] = 0;
    bgAdj_[1] = bgAdj_[2] = 0;
    adjOff_[1] = adjOff_[2] = 0;
    lastSpeed_ = rampFrames_ = 0;
    ramping_ = false;
    for (int8_t& d : deleteRange_) d = -1;
  }
  bool warpPending() const { return warpStage_ != 0; }
  bool overridesInput() const { return warpStage_ != 0; }
  uint16_t inputOverride() const { return input_; }
  std::string takeMessage();  // warp result message for the UI (empty if none)
  std::string debug() const;
  std::string verifyLines();  // development self-check (smastool verifywide)
  // Widescreen state kept in save states (see SmasGame::saveExt).
  void saveExt(std::vector<uint8_t>& out) const;
  bool loadExt(const uint8_t* p, size_t n, int version);
  // A state without that data: take it as saved with the game's own range.
  void assumeNarrowState() { spawn_.inactive(); }

private:
  Snes& snes_;
  int extL_ = 0, extR_ = 0;
  bool onePage_ = false;  // a single-page area (see frameStart)
  bool setPiece_ = false; // ... shown black either side (all but the airships' boss rooms)
  int pieceWall_ = -1;    // ... or walled in with this metatile (see startFrameView)
  bool pieceOpen_ = false;  // ... or carried on, dimmed (see startFrameView)
  bool pieceLatched_ = false;  // ... walled or not decided for this room (see startFrameView)
  int pieceWallLatch_ = -1;
  bool pieceOpenDone_ = false;  // ... an open one's rows worked out for this room (see startFrameView)
  bool pieceFramed_ = false;  // ... walled in by its frame carried on (not dimmed)
  bool battleRoom_ = false;   // ... a map battle's (dimmed: see beginLine)
  bool endPiece_ = false; // an end piece carried on is in the game's own 256 px (see fetchWideTile)
  bool carriesEnd(int side, int row) const;
  // an airship boss room's scenery (its cabin), per tile row of its picture:
  // the run of columns repeating by cabinP_ (0: none) from cabinS_
  uint8_t cabinP_[32] = {}, cabinS_[32] = {};
  void planCabin(const Ppu& ppu);
  bool gameplay_ = false;
  bool spade_ = false;  // the Spade bonus game, shown wide (see frameStart)
  bool inPlayer_ = false;
  // co-op (see smb3coop.cpp)
  bool coop_ = false, p2Valid_ = false, p2Drawn_ = false;
  bool ballChecked_ = false;  // the cannonball's full side-touch test passed (kBallHurt)
  int coopTurn_ = 0;
  unsigned areaStamp_ = 0, p2Area_ = ~0u;
  std::vector<uint8_t> p1_, p2_;
  uint8_t keep_[16] = {};
  uint8_t p1Oam_[48] = {}, p1OamHi_[12] = {}, p2Oam_[48] = {}, p2OamHi_[12] = {};
  uint8_t p2PadHi_ = 0, p2PadLo_ = 0;
  uint16_t p2Tiles_[192] = {}, p2Pal_[16] = {};
  uint64_t coopMask_[2] = {};  // Luigi's sprite entries (his tiles and colours)
  uint8_t used_[12] = {}, usedBytes_[48] = {};  // (the entries he was put in, as written)
  int usedCount_ = 0;
  uint8_t cardUsed_[6] = {}, cardBytes_[24] = {};  // LUIGI START! on the start card (see cardFrame)
  int cardCount_ = 0;
  void cardFrame();
  int oamNeed_ = 0, oamPut_ = 0;  // (his entries to show, and put: fewer, no room)
  int16_t p2X_[12] = {};  // his sprites' X on Mario's screen
  uint8_t& wvar(uint32_t a);
  void coopSync();
  bool coopExec();
  void coopOam(Ppu& ppu);
  std::string coopDebug() const;
  // objects meet the nearer player: Luigi's variables in for its update (see coopObjBefore)
  uint8_t objOwner_[8] = {};  // (1: Luigi, last frame)
  int objSwapped_ = -1;       // the slot running with Luigi's variables in
  std::vector<uint8_t> objSave_;
  void coopObjBefore();
  void coopObjAfter();
  void coopTakeOver();
  bool coopCanBring(int who) const;
  bool coopIdleSafe() const;
  int coopIdleOne() const;
  bool kingFrame_ = false;
  bool departure_ = false;
  uint8_t stageHold_ = 0;  // ($055A before his turn: see coopExec)  // (the airship's departure scene: see coopExec)  // (the King's room's scene ran this frame: see coopOam)
  uint8_t specKind_ = 0, coopTs_ = 0xff;  // (a special object's kind as Luigi's turn at it began; the tileset: see coopExec)
  void coopPlatBefore();
  bool coopPlatAfter();
  bool coopTiles();
  bool coopAutoCarry();
  bool carryPhase_ = false;
  uint8_t carryHold_[5] = {};
  int platPhase_ = 0, platSlot_ = 0;
  bool standHit_ = false, platHit1_ = false, platFarMario_ = false, tilePhase_ = false;
  std::vector<uint8_t> platBefore_, platAfter_, platNear_;
  uint16_t platCall_ = 0x90ec, platRet_ = 0x906d;  // (the object update's JSR target and return: see kObjUpdate)
  uint8_t pipeHold_[5] = {};
  void coopSpecBefore();  // (the same for the special objects: projectiles, the wand)
  void coopSwapIn(int id);
  void coopHoldCopy();
  // Luigi's death: his dying turns counted (-1: alive), then out until a life and Mario on his feet
  int p2Dead_ = -1;
  bool p2Wait_ = false;
  int musicBack_ = 0;  // frames until the area's music comes back after his death tune
  uint8_t musicTrack_ = 0;
  uint8_t deathBf_ = 0, deathFlag_ = 0, deathTrack_ = 0;  // (before one of his turns: see coopDeathBefore)
  uint8_t deathPSwitch_ = 0;
  uint8_t tickHold_[7] = {};  // (kTickOnce before his turn)
  bool coinHeld_ = false;
  uint8_t launchHold_ = 0;  // (a note block's launch before his turn: see coopExec)     // (a coin already taken this frame, before his turn: see kCoinCount)
  void coopDeathBefore();
  void coopDeathAfter();
  void coopReturn();
  void coopTick();
  // the camera (see coopCamera): on both, Luigi playing; the view's edges hold them
  bool camMid_ = false;
  int camAim_ = -1;      // the point the camera follows, eased toward its target (-1: none yet)
  unsigned camArea_ = ~0u;
  uint8_t camX_[2] = {};  // (Mario's X, while the camera's code sees the middle)
  bool camMidY_ = false;
  int ground_[2][2] = {{-1, 0}, {-1, 0}};  // where each last stood (X, Y; -1: not yet), for one resting to be brought to
  bool coopRests(int who) const;
  void coopBring(int who);  // (the resting one to where the other last stood)
  uint8_t pipeBefore_ = 0;  // [the pipe move] $0571 before Luigi's turn
  // The game's player slot holds either brother: Luigi after Mario died while he played on (see
  // coopSwapRoles); the second slot then has Mario, his controller and colours following him.
  bool roles_ = false;
  int titleCursor_ = 0, titlePick_ = -1;  // the title's choice (3: co-op)
  bool titleShown_ = false;
  uint16_t titleGlyph_[3] = {};  // the tiles C, O and - were put in
  bool titleExec(uint32_t pc);
  bool hudDrawn_ = false;  // the status bar shows both badges
  void boxFrame();         // (the map's WORLD box in co-op: both brothers, see smb3coop.cpp)
  bool boxShown_ = false;
  bool padSwapped_ = false;
  // ---- More Active Fireballs (smb3fire.cpp): the player's two projectile slots ($1CE1,X), and up
  // to kFbPool more fireballs kept here, each run through the game's own projectile code in slot 1.
public:
  static constexpr int kFbPool = 13;  // (+ the game's 2: 15 at once)
  static constexpr int kFbVars = 9;
  int fireballsInFlight() const;      // (testing)
private:
  bool fireExec();
  void fireClear();
  void fireSave(std::vector<uint8_t>& out) const;
  void fireLoad(const uint8_t*& p, size_t n);
  struct FbBall {
    uint8_t v[kFbVars];
    uint8_t owner = 0;  // (co-op: 1 = Luigi's, as projOwner_)
    bool live = false, done = false;
  } fb_[kFbPool];
  int fbCur_ = -1;
  uint8_t fbHold_[kFbVars + 2] = {};  // slot 1 as it was, its owner, $9B
  unsigned fbArea_ = ~0u;
  bool fireCheat_ = false;
  uint8_t projOwner_[2] = {};       // the player's fireballs/hammers ($1CE1,X): 1, the second slot's
  uint8_t projBefore_[2] = {};
  uint8_t padHi_[2] = {}, padLo_[2] = {};  // each controller's bytes last frame (for newly pressed)
  void coopProjBefore();
  void coopLevelOver();
  uint8_t lastTrack_ = 0;           // the music before a death's tune
  // The sound chip as it was before a death's tune (lastApu_: each frame's, taken
  // while none plays; apuSnap_: the one a death's tune ends back at, the music
  // going on from where it stopped rather than starting again).
  std::vector<uint8_t> lastApu_, apuSnap_;
  uint64_t coopMask2_[2] = {};      // the game's player's sprite entries, recoloured Luigi's
  uint16_t slotPal_[16] = {};
  void coopSwapRoles();
  void coopPadSwap();
  void playerPalette(int who, int suit, bool statue, uint16_t* out) const;
  uint8_t p2Press_ = 0;     // his controller's high byte, newly pressed this frame
  uint8_t objPad_[2] = {};  // (Mario's $17/$18, while an object meets Luigi)
  bool fireRet_ = false;    // Luigi's throw running (see coopFire)
  std::vector<uint8_t> fireSave_;
  uint8_t firePad_[2] = {};
  bool coopFire();
  uint8_t camY_[2] = {};  // (... and his Y, for the vertical scroll's)
  int p1Idle_ = 0, p2Idle_ = 1 << 20;  // frames since each controller was last touched
  bool coopCamera(uint32_t pc);
  void coopSave(std::vector<uint8_t>& out) const;  // (save states: Luigi, after the rest)
  void coopLoad(const uint8_t* p, size_t n);
  bool p2Plays() const;
  bool levelShown_ = false;  // a level fading in or out (see frameStart)
  bool levelLit_ = false;
  int irisR_ = -1;  // radius of the level-opening circle this frame (-1: none)
  int lastSpeed_ = 0, rampFrames_ = 0;  // auto-scroll speed last frame; picking up (see frameStart)
  bool ramping_ = false;
  bool vertical_ = false;
  bool hudAnchor_ = true;
  int cam16_ = 0;
  uint32_t tileTable_ = 0;
  // Level tile memory ($7E2000 and its high page $7E4000) as it was at this
  // frame's vblank, which is what the game's own tilemap shows this frame:
  // its tile changes reach VRAM at the next vblank, so the margins, drawn
  // from this copy, change on the same frame as the 4:3 picture.
  static constexpr int kTileMem = 15 * 0x1b0;
  uint8_t tileSnap_[2][kTileMem] = {};
  int tileMeta(int off) const { return tileSnap_[0][off] | (tileSnap_[1][off] ? 0x100 : 0); }
  // past a level's ends, per row (27) and end: carried on by p (from o
  // columns in), or p = 0: mirrored (see planFill)
  static constexpr int kFillRows = 27, kFillCols = 8;
  struct PieceRow {
    int p = 0, tile = -1;  // a pattern of p tiles from the edge, or this tile; neither: nothing
    int a = 0;             // ... the column it starts from (the edge's, or inside a frame)
  };
  PieceRow pieceRow_[2][kFillRows];
  struct FillRow {
    int8_t p = 0, o = 0;
    int16_t tile = -1;  // or this metatile throughout (solid ground past a cliff)
    bool wall = false;  // a wall's blocks carried on: kept, not taken for coins and bricks
    int8_t from = -1;   // or as row `from` is (a wall's blocks above it, carried down)
    bool open = false;  // mirrored over a floor: nothing to stand on (see planFill)
  };
  bool hardTile(int t) const;  // solid all round, as the game tells it
  bool standTile(int t) const;  // ... solid from above at least
  FillRow fill_[2][kFillRows];
  bool rareCol_[2][kFillCols] = {};  // mirrored columns with a once-only metatile
  int8_t rareLo_[2][kFillCols] = {}, rareHi_[2][kFillCols] = {};  // ... its object's rows there
  uint16_t metaCount_[512] = {};     // occurrences of each metatile in the level
  int16_t rowBg_[2][kFillRows] = {};  // per row next to the end: what is round the coins and blocks
  int16_t startFace_ = -1;  // the wall before an enclosed start: its face's tile, turned round (see planFill)
  int faceTop_ = 0, faceBottom_ = -1;  // ... in these rows
  int16_t startCorner_ = -1;  // ... and its corner into the floor, the row under them
  // the shared item tiles: coins ($40) and blocks (? blocks, bricks, note
  // and wood blocks: $60-$7F)
  static bool isItem(int t) { return t == 0x40 || (t >= 0x60 && t <= 0x7f); }
  // pipes ($AD-$BF in every level tileset: tops, bodies, horizontal ends)
  static bool isPipe(int t) { return t >= 0xad && t <= 0xbf; }
  void planFill();
  std::map<uint32_t, uint8_t> original_;
  // warp
  int warpStage_ = 0, warpTimer_ = 0, warpWorld_ = 0, warpLevel_ = 0;
  int swapPending_ = 0;  // level select's airship / Bowser's castle (its code): swapped in as a panel's level loads (see onExec)
  bool heldMirror_[2] = {};  // past this end: the held screen turned round (see startFrameView)
  int swapWait_ = 0;     // ... frames since (it lapses: see updateWarp)
  uint16_t input_ = 0;
  std::string message_;

  uint8_t ram(uint16_t a) const { return snes_.wram[a]; }
  int ext_ = 0;  // requested margin per side (the view window may shift it)
  int want_ = 0;
  bool active_ = false;
  int viewExt_ = 0;
  bool viewInit_ = false;
  int autoL_ = -1;       // auto-scroll: the view's fixed left margin (-1: not scrolling)
  int shownL_ = 0;       // left margin shown last frame
  // Walls the player can't get past either side (see findWalls): beyond
  // them, from `from` (x; left: below it, right: from it), the wall's columns
  // carried on, `start` + multiples of `period`; sprites are drawn only up to
  // `show` (x).
  struct Beyond {
    bool on = false;
    int from = 0, start = 0, period = 1, show = 0;
    int tile = -1;  // or this metatile (beyond a thin wall: the walls' material)
    int edge = 0;   // the wall's outer edge (x): past it, what the wall hides
    // ... but in rows with a floor through the wall, its columns from
    // bandS[row], bandP[row] of them (1-2; 0: none), carried on
    int16_t bandS[kFillRows] = {};
    int8_t bandP[kFillRows] = {};
  };
  Beyond beyond_[2];
  bool boxed_ = false;  // ... and a room between them, the view centred on it
  int boxCentre_ = 0;
  void findWalls();
  void findWallsIn();
  bool wallsStale_ = false;  // a new area: the walls found are the last one's (see findWalls)
  bool easing_ = false;  // moving back to the normal view after auto-scroll
  bool roomGlide_ = false;  // ... or into or out of a room (see findWalls): steadily
  int easeCam_ = 0;      // camera last frame (easing follows its motion)
  void currentSplit(int* l, int* r) const;
  bool shellBump(bool edge);  // exec hooks: a kicked shell bumps blocks in the margins too
  bool autoscrollBounds();  // exec hooks: auto-scroll player bounds (wide),
  bool edgeDeath();         // pushed off the right side,
  bool offscreenGate();             // off-screen tests before interactions
  bool viewGate(uint32_t pc);       // more of them, with the enemies active in full view
  bool inWideView(int slot) const;  // slot's object horizontally inside the wide view
  // Auto-scroll: objects the wide range spawns before the 4:3 game would, kept
  // on the 4:3 game's timeline (see kObjUpdate in smb3.cpp).
  bool holdBefore();  // true: skip the object's update this frame
  void holdAfter();
  enum : int8_t { kFree, kWaiting, kAhead, kLast };  // held_
  struct Hold {
    uint8_t state;
    uint8_t vars[7];  // kObjVars, as its initialisation left them
    int16_t col;      // its spawn column
  };
  void captureHold(int slot);
  int framesToSpawn(int col) const;
  int8_t held_[8] = {};
  bool holdCaptured_[8] = {};
  Hold hold_[8] = {};
  // The game has five slots for level objects. An object waiting beyond the
  // view gives up its slot and is spawned again once it comes into view; a
  // spawn that found no free slot is tried again (see park()).
  struct Parked {
    uint8_t idx;  // its level object (spawned flag at $1B10 + idx)
    Hold start;
  };
  static constexpr int kMaxParked = 16;
  Parked parked_[kMaxParked] = {};
  int parkedCount_ = 0;
  struct Retry {
    uint8_t idx;
    int16_t col;
    uint8_t tries;
  };
  static constexpr int kMaxRetry = 8;
  Retry retry_[kMaxRetry] = {};
  int retryCount_ = 0;
  bool returning_[8] = {};  // spawned again after being parked
  bool optional_[8] = {};   // shown in the margins before the 4:3 game has it
  uint8_t spawnerMade_ = 0;  // slots the level's spawner has just filled (bit per slot; see holdBefore)
  bool optNormal_[8] = {};  // ... in a level with normal scrolling
  int8_t deleteRange_[5] = {-1, -1, -1, -1, -1};  // each object's off-screen delete range (see kDeleteRange)
  bool park(int slot);
  bool dueColumn(int* col);
  bool evictOptional();
  bool evictHidden(int need);
  bool evict(int s, bool gone);
  bool gone43(int s, int vx) const;
  int outside(int vx) const;
  void leadIn(int slot);
  void spawnFailed();
  void spawnedOne();
  void objectDeleted();
  void addRetry(int idx, int col);
  void forgetParked() {
    parkedCount_ = retryCount_ = 0;
    for (bool& b : returning_) b = false;
    for (bool& b : optional_) b = false;
    for (bool& b : optNormal_) b = false;
  }
  uint8_t prevState_[8] = {};
  int16_t lastY_[8] = {};  // each object's Y after its last update
  bool holdInit_ = false;
  bool adoptPending_ = false;
  // True X of the sprites objects write (see onWrite), for the next upload.
  int drawing_ = -1, drawingRel_ = 0;
  // Sprites shown this frame, by the object that drew them (slot + 1; 0:
  // none), and each one as it left it (4 bytes, high byte); and the object a
  // sprite is kept from (slot + 1) (see onWrite)
  uint8_t oamOwner_[128] = {}, oamKept_[128] = {};
  bool oamWritten_[128] = {};  // written this frame (by anything)
  uint8_t oamSnap_[128][5] = {};
  int16_t oamSnapX_[128] = {};
  int16_t oamX_[128] = {};
  bool oamXSet_[128] = {};
  bool goalShown_[128] = {};  // sprites the goal walk keeps (see goalWalk)
  bool noDamage_ = false;      // the Invincibility cheat is on (see kHurtGate)
  bool goalMsg_ = false;       // "COURSE CLEAR!" on BG3 (see startFrameView)
  void goalWalk();
  bool goalStop(bool on);
  void adoptSweptObjects();
  bool autoscroll() const;
  void startFrameView(Ppu& ppu);
  // background layer speeds (BG2/BG3 relative to the camera), per tileset
  void learnLayerSpeeds(Ppu& ppu);
  double layerSpeed_[3] = {1.0, 0.5, 0.25};
  double knownSpeed_[16][3];
  int speedTileset_ = -1;
  int sumCam_[3] = {}, sumLayer_[3] = {};
  int speedPrevCam_ = 0, speedPrevHofs_[3] = {};
  bool speedPrevOk_ = false;
  int16_t bgAdj_[3] = {};
  int line0Cam_ = 0;  // the playfield's camera on the line being drawn (see beginLine)
  // verifyLines (development self-check)
  bool verifyArmed_ = false;
  int verifyCount_ = 0, verifyBad_ = 0, verifyRowLo_ = 999, verifyRowHi_ = -1, verifyX_ = 0, verifyRam_ = 0, verifyVram_ = 0;
  struct VerifyTile {
    int x, y, m;
    uint16_t ram, vram;
  };
  std::vector<VerifyTile> verifyTiles_;  // distinct mismatched tiles (first few)
  void verifyLine(Ppu& ppu, int row);
  int adjOff_[3] = {};           // kept from an earlier layer speed (see startFrameView)
  double adjSpeed_[3] = {};      // the speed bgAdj_ was last worked out with
  void patch(uint32_t adr, uint8_t v);
  void patch16(uint32_t adr, uint16_t v) {
    patch(adr, v & 0xff);
    patch(adr + 1, v >> 8);
  }
  void unpatchAll();
  void patchSpawn(int l, int r, int sweepHi = -1);
  bool sweeping_ = false, swept_ = false;
  int sweepCam_ = 0;  // the camera the level-load sweep started from
  // Level controllers held back to their 4:3 spawn point (see holdControl).
  bool heldControl_[0x30] = {};
  bool holdControl();
  bool dueControl(int* col);
  void forgetHeldControls() {
    for (bool& b : heldControl_) b = false;
  }
  SpawnCatchUp spawn_;
  void trackSpawnGrowth();
  void catchUpSpawn();
  void updateWarp();
  bool findPanel(int level, int* y, int* xhi, int* x) const;
 public:
  static constexpr int kWarpAirship = 20;  // level select's code for the world's airship
  static constexpr int kWarpExtra = 30;    // ... and for kExtraLevels[i]: 30 + i
  // Levels with no numbered or fortress panel (a map's own pictures, or map
  // sprites: the tanks, the Piranha Plants), from the worlds' map level tables
  // ($21:D86B on): its world (0-7), name, layout, objects and tileset.
  struct ExtraLevel {
    int world;
    const char* name;
    uint32_t lay, obj;
    uint8_t tileset;
  };
  static const ExtraLevel kExtraLevels[];
  static const int kExtraCount;
 private:
  void openWorldTo(int number, int y, int xhi, int x);  // number: 1-10, 0 for a fortress
};
