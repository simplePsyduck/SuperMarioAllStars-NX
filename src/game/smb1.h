// Widescreen / HUD / sprite logic for the SMB1 engine (Super Mario Bros. and
// The Lost Levels share it; each title has its own code bank, located by
// byte signatures so both use the same implementation).
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "core/snes.h"

class Smb1Engine {
public:
  // bankLo/bankHi: ROM banks searched for the title's code.
  Smb1Engine(Snes& snes, uint8_t bankLo, uint8_t bankHi);
  bool ok() const { return ok_; }

  // Called once per frame before emulation. `want` = requested symmetric
  // margin; `active` = extend enemy activity to the full view.
  void frameStart(int want, bool active, bool hudAnchor);
  int extL() const { return extL_; }
  int extR() const { return extR_; }
  bool gameplay() const { return gameplay_; }

  void markExec();                         // register exec-hook PCs
  bool onExec(uint32_t pc);                // exec hook dispatcher
  void onWrite(uint32_t adr, uint8_t v, uint32_t pc);  // WRAM write hook (pc: the writing instruction)
  void beginLine(Ppu& ppu, int row);
  int fetchWideTile(Ppu& ppu, int layer, int tx, int ty, int screenX, uint16_t* entry);
  void onVramWrite(Ppu& ppu, uint16_t adr);
  void reset();
  // Widescreen state kept in save states (see SmasGame::saveExt).
  void saveExt(std::vector<uint8_t>& out) const;
  // running: this title is the one running (the PPU's sprite positions are its).
  void loadExt(const uint8_t* p, size_t n, bool running);
  void seedColumns();
  std::string debug() const;

  // ---- cheats (bit flags, see kCheat*)
  enum {
    kCheatLives = 1 << 0,
    kCheatStar = 1 << 1,
    kCheatNoDamage = 1 << 2,
    kCheatFire = 1 << 3,
    kCheatTime = 1 << 4,
    kCheatMoonJump = 1 << 5,
    kCheatFireballs = 1 << 6,  // More Active Fireballs: up to 15 at once (see smb1fire.cpp)
    kCheatFireToBig = 1 << 7,  // Fire Mario goes to large Mario when hit (see injExec)
    kCheatCarry = 1 << 8,      // Pick Up & Carry Koopa Shells (see smb1carry.cpp; cheat ids 40 / 41, see smas.cpp)
  };
  void applyCheats(uint32_t flags);

  // ---- two-player co-op (experimental, Super Mario Bros. only; see smb1coop.cpp)
  void setCoop(bool on);
  bool coopActive() const { return coopOk_ && coop_; }
  // Row 0 of each frame: Luigi's sprites take his colours (see smb1coop.cpp).
  void coopLine(Ppu& ppu, int row);
  void coopPoke(uint16_t a, uint8_t v);  // (testing: one of Luigi's variables)
  bool coopMidTurn() const;               // (Luigi's variables in: see smb1coop.cpp)
  void coopCheats(uint32_t f);            // (the cheats for the other player too)
  void coopBringIdle();
  void coopIdleSafe();
  void coopP2OamClear();
  uint32_t coopLoop_ = 0;  // a castle loop's test (see coopInit)
  uint32_t coopMenu_[3] = {};  // the menus' first-controller reads (see coopInit)
  uint8_t menuPress2_ = 0, menuHeld2_ = 0;
  int metatile(int px, int py) const;
  // For the achievements: Luigi is the game's player (he took the axe, he's
  // on the flagpole); counts that go up as Luigi kept aside rides the
  // flagpole, and as the player kept aside is hurt or loses a life.
  bool coopLuigiLive() const { return coopActive() && luigiLive_; }
  // Character colour variants (Mario's, Luigi's: game/palette.h), and
  // whether the game's player is Luigi (co-op's swap, or a 2-player game's /
  // The Lost Levels' Luigi: [CurrentPlayer] $0753).
  void setCharacterColours(int mario, int luigi) { marioVar_ = mario, luigiVar_ = luigi; }
  bool slotIsLuigi() const { return coopActive() ? luigiLive_ : snes_.wram[0x0753] != 0; }
  uint32_t coopLuigiPoles() const { return luigiPoles_; }
  uint32_t coopP2Hurts() const { return p2Hurts_; }

  // ---- level select
  int worldCount() const { return worlds_; }
  void requestWarp(int world, int level);
  void cancelWarp() {
    warpPending_ = false;
    warpTimer_ = 0;
    inputOverride_ = 0;
  }
  bool warpPending() const { return warpPending_; }
  // The area number ($0760) a level (0-3) of a world starts in: worlds with five areas have an intro
  // area ahead of level 2. -1 if the world isn't known (the ROM's tables weren't found).
  int areaOf(int world, int level) const {
    if (world < 0 || world >= worlds_ || level < 0 || level > 3) return -1;
    return areaCount(world) >= 5 && level >= 1 ? level + 1 : level;
  }
  // Buttons the engine wants pressed this frame (while starting a warp).
  uint16_t inputOverride() const { return inputOverride_; }
  bool overridesInput() const { return warpPending_; }

private:
  Snes& snes_;
  bool ok_ = false;
  // signature addresses (24-bit)
  uint32_t spawnOp_ = 0, despawnLOp_ = 0, despawnROp_ = 0;
  uint32_t blinkLda_ = 0;    // PlayerGfxHandler: LDA InjuryTimer (blink while invincible)
  uint32_t firebarLda_ = 0;  // ProcFirebar: LDA Enemy_OffscreenBits (see kSigFirebar)
  uint32_t firebarWrap_ = 0; // DrawFirebar_Collision: CMP #$59 (see kSigFirebarWrap)
  uint32_t firebarEnd_ = 0;  // ProcFirebar's RTS
  uint8_t firebarTimer_ = 0;
  int firebarPivot_ = 0;  // its pivot's position relative to the camera
  bool firebarHeld_ = false;  // TimerControl set for a bar in the margin (see onExec)
  uint32_t cheats_ = 0;
  // Sprite drawing code reading off-screen bits (see kDrawSites)
  struct DrawSiteAt {
    uint32_t pc;
    uint16_t relAddr;
  };
  std::vector<DrawSiteAt> drawSites_;
  uint16_t relAddr_ = 0;   // SprObject_Rel_XPos address written last
  uint32_t gameHiFrame_[128] = {};  // frame in which the game wrote each sprite's $0C00 byte
  uint32_t ourHiFrame_[128] = {};   // ... in which its X bit 8 was chosen here (see onWrite)
  bool ourHi_[128] = {};
  bool firebarDraw_ = false, firebarRel_ = false;  // a firebar is being drawn
  uint32_t firebarColl_ = 0, firebarCollEnd_ = 0;  // a flame's meeting with the player, and its end
  int flameX_ = 0;                                // the flame's true screen X (see firebarWrap_)
  bool flameValid_ = false, relHeld_ = false;
  uint8_t relHold_ = 0;
  bool relValid_ = false;  // curRel_ belongs to that object
  // BG2 hill panorama: the game keeps it whole in WRAM and streams columns
  // into VRAM as the (half speed) layer scrolls; margins are drawn from it.
  bool bg2Source_ = false;
  int bg2Votes_ = 0;
  bool bg2Fresh_ = true;
  int bg2Period_ = 0;  // panorama length in tile columns (see panoramaPeriod)
  int bg2Off_ = 0;     // panorama column shown at layer column 0 (whole pages)
  uint64_t bg2Streamed_ = 0;  // VRAM columns of hill columns streamed since last frame
  // Night levels (3-1, 6-2) don't stream the layer: the game draws its 512 px
  // map once and the hardware wraps it, so the margins show VRAM. (Their WRAM
  // panorama is mostly stale beyond its first 64 columns.) Decided once the
  // camera has moved 160 px into the area with no column streamed (columns
  // come in strips of 8, one every 128 px of camera).
  bool bg2Static_ = false, bg2SeenStream_ = false;
  int bg2CamStart_ = 0;
  uint32_t bg2Fits_[8] = {};  // page offsets each of the last 8 streamed columns fits
  bool bg2Looped_ = false;    // a castle maze loop set the offset (see trackParser): no more voting in the area
  int bg2FitPos_ = 0;
  int panoramaPeriod() const;
  void checkBg2Source();
  // Left of the level's start: the panorama mirrored about its first column
  // (where its end doesn't join its start) instead of looped.
  bool bg2Mirror_ = false;
  int seamOff_ = -1, seamPeriod_ = -1;
  bool bg2StartSeam() const;
  uint16_t bg2Entry(int col, int row) const;
  uint32_t cloudTiles_ = 0;  // metatile -> 4 tilemap words table for the cloud layer
  bool cloudStream_ = false;
  uint32_t worldTbl_ = 0, areaTbl_ = 0;  // FindAreaPointer tables
  int worlds_ = 0;
  // ---- More Active Fireballs (smb1fire.cpp): the game's two fireball slots, and up to
  // kFbPool more kept here, each run through the game's own FireballObjCore in slot 1.
public:
  static constexpr int kFbPool = 13;  // (+ the game's 2: 15 at once)
  static constexpr int kFbVars = 16;
  int fireballsInFlight() const;      // (testing)
  std::string fireDebug() const;      // (testing: each fireball's state, Y high, Y, page, X)
private:
  void fireInit(uint8_t bankLo, uint8_t bankHi);
  bool fireExec();
  void fireClear();
  void fireSave(std::vector<uint8_t>& out) const;
  void fireLoad(const uint8_t*& p, size_t& n);
  uint32_t fbCore_ = 0, fbRet_ = 0;
  // Fire Mario goes to large Mario when hit: ForceInjury's STA PlayerStatus, its start of the shrink
  // (LDA #$0A), and its way out (LDX ObjectOffset; RTS); see injExec.
  uint32_t injStore_ = 0, injShrink_ = 0, injExit_ = 0;
  // Pick Up & Carry Koopa Shells (smb1carry.cpp): PlayerEnemyCollision's touch of enemy X ($03:DE90),
  // its kick of a still shell ($03:DEE7), its way out (RTS, $03:DF14).
  uint32_t carryTouch_ = 0, carryKick_ = 0, carryExit_ = 0;
  uint16_t carryTouchBits_ = 0;  // ([Enemy_CollisionBits]: bit 0, touching the player last time; SMB1 $0481)
  int carried_ = -1;  // the enemy slot held (-1: none)
  void carryInit(uint8_t bankLo, uint8_t bankHi);
  bool carryExec(uint32_t pc);
  void carryPlace();  // (its update about to run: beside the player)
  void carryFrame();  // (each frame: let go of it when it can't be held, throw it when run is let go)
public:
  int carriedShell() const { return carried_; }  // (testing)
  std::string carryDebug() const {
    char b[96];
    snprintf(b, sizeof b, "touch %06X kick %06X exit %06X bits %04X", carryTouch_, carryKick_, carryExit_, carryTouchBits_);
    return b;
  }
private:
  bool injFire_ = false;
  bool injExec(uint32_t pc);  // FireballObjCore; just after ProcFireballs' second call of it
  struct FbBall {
    uint8_t v[kFbVars];
    bool live = false, done = false;
  } fb_[kFbPool];
  int fbCur_ = -1;                    // the pool's fireball in slot 1 now (-1: none)
  uint8_t fbHold_[kFbVars + 1] = {};  // slot 1 as it was, and its sprite offset
  uint32_t fbArea_ = 0xffffffff;      // the area the pool's fireballs are in
  bool warpPending_ = false;
  int warpWorld_ = 0, warpLevel_ = 0, warpTimer_ = 0;
  uint16_t inputOverride_ = 0;
  int areaCount(int world) const;
  void applyWarp();
  std::map<uint32_t, uint8_t> original_;  // original ROM bytes of patched locations

  int extL_ = 0, extR_ = 0;
  bool gameplay_ = false;
  bool lockedRoom_ = false;  // in a single-screen room
  // A single-screen room walled in on the left (the coin rooms) is shown wide,
  // its wall carried on over both margins (from roomRight_ on the right).
  bool padRoom_ = false;
  int roomRight_ = 0;
  bool openRoom_ = false;  // ... one that doesn't (the scenes before x-2's pipe): centred, its ground carried on
  int areaFrames_ = 0;       // frames of play since the area started (to 255)
  int areaLeft_ = 0;         // the area's left end: the camera when it started (see frameStart)
  bool loadedMidArea_ = true;  // a state was loaded (or reset): the area began further back
  bool areaLeftLoaded_ = false;  // ... and areaLeft_ came with it
  int playerLeftLimit() const;
  uint32_t chkPOffscr_ = 0;  // ChkPOffscr: STA $00 after the player's GetXOffscreenBits
  uint32_t posForScroll_ = 0;  // STA $05 after STA Player_Pos_ForScroll (see kSigPosForScroll)
  uint32_t scrollAmt_ = 0;     // ScrollHandler's LDY Player_X_Scroll (see onExec)
  uint32_t boundBox_ = 0, boxTable_ = 0, collide_ = 0, enemyBoxOff_ = 0;  // see kSigBoundBox
  uint32_t fireballOff_ = 0;  // see kSigFireballOff
  uint32_t hammerThrow_ = 0, hammerGone_ = 0, hammerKeep_ = 0, hammerColl_ = 0;  // see kSigHammerThrow ..
  static constexpr int kBoxes = 32;
  int boxL_[kBoxes] = {}, boxR_[kBoxes] = {};  // each object's box in level X, this frame
  uint32_t boxFrame_[kBoxes] = {};
  bool pipeFade_ = false;    // play just ended (pipe, death, level end): wide until the screen is black
  int fadeFrames_ = 0;
  bool card_ = false;        // the card between levels is up (see frameStart)
  bool wasVictory_ = false;  // VictoryMode ($0770 = 2) last frame: Bowser beaten
  int victoryL_ = -1;        // the left margin, eased to the centre over it (-1: not in it)
  int titleL_ = -1;          // the title screen's left margin: centred at the start, eased (-1: not on it)
  bool enemyRange_ = false;  // the wide enemy range is patched in
  // The wide range spawns enemies past what the area parser has built, where
  // the collision map still holds the level 512 px back: a new walker waits,
  // out of view, until the level under it is built (see holdEnemy).
  uint32_t enemyCall_ = 0, enemyRet_ = 0;  // JSR EnemiesAndLoopsCore in the enemy loop, and after it
  uint8_t preFlags_[10] = {};  // enemy flags before that call (to see spawns)
  uint16_t fresh_ = 0;         // slots holding a walker not yet released
  bool holdEnemy(int x);
  void placeLate(int k);  // a late spawn in the visible margin starts beyond the view (see smb1.cpp)
  uint8_t solidFrom_[4] = {0xff, 0xff, 0xff, 0xff};  // CheckForSolidMTiles' table (see constructor)
  bool solidAt(int x, int y) const;
  // Sprite positions in 16 bits: a 9-bit OAM X is ambiguous at 21:9 (the game
  // draws objects up to ~420 px right of its screen, which would wrap into
  // the left margin). Recorded as the buffer is written, latched at its DMA.
  uint32_t oamUpload_ = 0;
  int16_t oamX_[128] = {};
  uint8_t oamXSet_[128] = {};
  // Sprites the game places with its own 16-bit code (not via an object's
  // relative position): the position it just worked out (SEC; SBC $42).
  std::vector<uint32_t> rel16Sites_;  // the instruction after each SBC $42 (sorted)
  int ref16_ = 0, ref16Writes_ = 0;
  uint32_t drawSeq_ = 0, ref16Seq_ = 0, relSeq_ = 0;  // which reference came last
  uint32_t groupPlaced_ = 0;              // enemy group start set ($02/$03), see kSigGroup
  uint32_t springSlot_ = 0;               // the jumpspring's object slot chosen, see kSigSpring
  void rescueSpring();                    // a jumpspring left in slot 5 moves to a free enemy slot
  uint32_t bowserBig_[2] = {};            // Bowser's high-table stores that keep 16x16, see kSigBowserHi
  std::vector<uint32_t> edgeSpawnOps_;    // ADC #$20 operands of edge spawns, see kSigEdgeSpawn
  bool hudAnchor_ = true;
  int cam16_ = 0;
  // sprite X fix-up
  int curRel_ = 0;
  int relBy_[7] = {};  // each Rel_XPos ($03AD-$03B3) as last worked out, valid this frame (see onWrite)
  uint32_t relByFrame_[7] = {};
  uint8_t relV_[7] = {};  // (the value the game wrote for each, then)
  int curWrites_ = 0;  // OAM X writes remaining for the current object
  // BG1 column tracking: world tile column held by each VRAM map column
  int colWorld_[64];
  // Copy of every BG1 column the game uploads, kept after VRAM reuses the
  // slot (the 64-column VRAM ring holds only ~140 px behind the camera).
  static constexpr int kShadowCols = 128;
  int shadowCol_[kShadowCols];
  uint16_t shadow_[kShadowCols][32];
  int lastParser_ = 0;
  int hudHofs_ = 0, hudVofs_ = 0;  // BG3 scroll of the status line (line 0)
  void clearShadow();
  bool colEntry(int wc, int row, uint16_t* e) const;  // a built BG1 column's tile (VRAM or shadow)
  // Left of where the area began (nothing built there) the margin repeats the
  // area's commonest 16-px column (its plain floor and ceiling): see makeFill.
  uint16_t fill_[2][32] = {};
  bool fillValid_ = false;
  bool fillWall_ = false;  // the fill is the wall the area opens with
  void makeFill();
  // ... and its metatiles per collision-map row, for objects that go there
  // (BlockBufferCollision's read of the map, see onExec).
  uint8_t fillMt_[13] = {};
  bool fillMtValid_ = false;
  uint32_t bbRead_ = 0, bbXAdder_ = 0;

  // Two-player co-op: the game's player routines run a second time each frame
  // with Luigi's variables swapped in (see smb1coop.cpp).
  bool coop_ = false, coopOk_ = false;
  uint32_t coopCall_ = 0, coopAfter_ = 0;  // GameCoreRoutine: JSR GameRoutines, and after it
  uint32_t coopDraw_ = 0, coopDrawEnd_ = 0;  // JSR GetPlayerOffscreenBits .. after JSR PlayerGfxHandler
  int coopMove_ = 0, coopDrawPhase_ = 0;  // 1: Luigi's turn is running
  uint32_t coopEnemyAt_ = 0, coopPEC_ = 0;  // after the enemy loop; PlayerEnemyCollision
  uint32_t coopDeathSmall_ = 0;             // the injury routine's small-player death (see coopExec)
  bool afterEnemies_ = false;               // this frame's enemy loop has run (coopEnemyAt_)
  uint32_t coopLeftHi_ = 0;  // the player's left column's X bit 8 set (see smb1coop.cpp)
  uint32_t coopPack_ = 0;    // the OAM high table packed (the frame's sprites drawn)
  // Each player's sprites as drawn this frame (0 the game's player, 1 the other)
  uint8_t sprSave_[2][8][4] = {}, sprHi_[2][8] = {}, sprXSet_[2][8] = {};
  int16_t sprX_[2][8] = {};
  int sprBase_[2] = {};
  uint32_t sprFrame_[2] = {~0u, ~0u};
  void saveSprites(int p, uint8_t offset);
  // The second player in save states (and rewind): after the rest (see smb1coop.cpp)
  void coopSave(std::vector<uint8_t>& out) const;
 public:
  // The title screen's co-op choice, once a game starts from it (-1: none since).
  int takeTitlePick() {
    int p = titlePick_;
    titlePick_ = -1;
    return p;
  }
  void titleFrame();  // (after each frame: the choice drawn, kept, taken)
  void hudFrame();    // (... and the status bar's name: MARIO & LUIGI in co-op)
 private:
  void coopLoad(const uint8_t* p, size_t n);
  uint32_t coopFire_ = 0, coopFbCore_ = 0;  // after JSR ProcFireball_Bubble; FireballObjCore
  uint32_t coopWhirl_ = 0, coopWhirlRet_ = 0;  // ProcessWhirlpools' loop; after its JSR (see coopExec)
  uint32_t bubbleTimes_ = 0;  // BubbleTimerData (see coopBubble)
  uint8_t p2Bubble_ = 0;      // Luigi's [AirBubbleTimer]
  uint8_t dustHold_[16] = {};  // the game's player's skid dust's OAM entries, around Luigi's turn
  void coopBubble();
  int coopWhirlPhase_ = 0;
  int coopFirePhase_ = 0;
  int coopFireQ_[3] = {}, coopFireN_ = 0, coopFireI_ = 0;  // FireballObjCore calls to make (bit 7: Luigi's turn)
  bool coopFireSwap_ = false;
  uint8_t p2PrevAB_ = 0;  // Luigi's A and B of the frame before
  bool p2Known_ = false;  // Luigi has his own power-up (kept by p2Sync)
  void p2Lose();
  void p2Return();
  bool p2CanReturn() const;
  bool p2Wait_ = false;  // lost a life: back beside the other once he's on his feet
  // Objects that meet the player in their own routines, run again for the
  // other player (see smb1coop.cpp): lifts, the jumpspring, firebars
  void coopObjBefore(int x);
  bool coopObjAfter();
  void restoreLowRam(const std::vector<uint8_t>& from);
  int coopObjPhase_ = 0, coopObjSlot_ = 0;
  std::vector<uint8_t> objBefore_, objAfter_;
  uint8_t objAB_ = 0, objP2Flag_ = 0xff, objP2Spring_ = 0;
  uint8_t p2Plat_[10] = {}, p2PlatId_[10] = {};  // his [PlatformCollisionFlag]s
  uint16_t collBits_ = 0;                        // [Enemy_CollisionBits]: bit 0 each player's own
  uint8_t p2Coll_[10] = {}, p1Coll_[10] = {};
  // The hammers' meeting with the player (PlayerHammerCollision), for both
  uint32_t coopHammer_ = 0, coopHammerRts_ = 0;
  uint16_t hamFlag_ = 0;  // [Misc_Collision_Flag]
  int coopHamPhase_ = 0, coopHamSlot_ = 0;
  uint8_t p2Ham_[16] = {}, hamP1_ = 0;
  uint8_t hamPre_[8] = {};
  void p2Timers();
  bool holdScroll(int x, int pad) const;
  bool coopStands(bool p2) const;
  uint8_t stoodAt_[2][5] = {};  // where the game's player (0) / Luigi (1) last stood: $78 $219 $237 $BB $28
  bool stoodOk_[2] = {};
  int stoodArea_ = -1;
  // (frames each controller untouched; one never touched yet counts as idle: else the screen
  // waited 3 s at its right edge for a Luigi nobody plays, see holdScroll)
  uint32_t idle_[2] = {1u << 20, 1u << 20};
  uint32_t still_[2] = {1u << 20, 1u << 20};  // (the same, capped)
  bool idleSafe_[2] = {};  // by controller: left standing, can't be hurt (see coopIdleSafe)
  uint32_t coopVicDraw_ = 0, coopEndStart_ = 0;
  bool start2_ = false, prevStart2_ = false;  // the victory scene's player drawing (see coopExec)
  int vicPhase_ = 0;
  bool p2Leads_ = false;  // the other player leads this frame (moves the screen)
  bool vicStarted_ = false, endingLuigi_ = false;
  int lastMe_ = 0, vicStill_ = 0, vicLastMe_ = 0;
  bool coopEdge_ = false;  // the screen held for the other this turn: the right edge is the view's
  uint32_t coopThanks_ = 0;  // Toad's thanks, chosen by [CurrentPlayer] (see coopExec)
  void coopFrame();
  uint32_t coopWalk_ = 0;  // AutoControlPlayer (see coopAfter_)
  int doorWait_ = 0, doorX_ = 0, sceneStuck_ = 0, sceneLastX_ = 0, sceneHop_ = 0;  // frames each controller has been left alone
  uint64_t altBits_ = 0, alt2Bits_ = 0;  // OAM entries recoloured this frame (see coopLine)
  uint64_t cardBits_[2] = {}, cardShown_[2] = {};  // the level card's Luigi head (see coopPack_)
  uint32_t cardFrame_ = ~0u;
  bool cardLuigi_ = false;  // (the game's head is Luigi's: The Lost Levels, his game)
  uint32_t coopStar_ = 0, coopStarMusic_ = 0;  // the star's end; the area's music back (see coopExec)
  bool starMusic_ = false;
  bool deathTune_ = false;  // his death's tune to play (see coopStar_), then the area's music back
  int musicBack_ = 0;
  // The sound chip before a death's tune (lastApu_: each frame's while none is
  // queued or plays): the music goes on from there after it (apuSnap_).
  std::vector<uint8_t> lastApu_, apuSnap_;
  uint8_t lastTrack_ = 0, musicTrack_ = 0;
  uint32_t luigiPoles_ = 0, p2Hurts_ = 0;  // (see coopLuigiPoles)
  uint32_t sceneFrame_ = 0;  // the last frame with a scene turn (the flag, the castle walk)
  uint8_t lastP2Inj_ = 0;  // his injury timer as last seen (a hit starts it)
  uint8_t lastInterval_ = 0;  // [IntervalTimerControl] as last seen (his timers: see coopExec)
  int coopEnemy_ = -1;                      // the enemy Luigi is meeting (-1: none)
  bool p2Dead_ = false;
  bool p2Out_ = false;  // out of lives: Luigi sits out until the next area or try
  uint8_t preTurn_[8] = {}, preLoop_[8] = {};  // (a death's set-up, undone: see smb1coop.cpp)
  bool p2Alive(bool needLife = true) const;
  void swapRoles();
  int marioVar_ = 0, luigiVar_ = 0;
  bool boxAfterSwap_ = false;  // the game's player box is the other brother's (see swapRoles)
  bool restWait_ = false;      // the waiting brother is an idle one kept out of a hole, no life lost (see coopBringIdle)
  bool luigiLive_ = false;  // Luigi is the game's player (Mario kept aside): see swapRoles
  bool liveCtrl_ = false;   // the game's player is on the second controller
  // The title's third choice, co-op (see smb1title in smb1coop.cpp): the
  // cursor's row (0, 1: the game's; 2: co-op), and the pick for SmasGame.
  uint32_t titleInput_ = 0;
  uint32_t livesSet_ = 0;  // just past the lives' start from the save (STA NumberofLives): co-op's at least 10
  int titleCursor_ = 0, titlePick_ = -1;
  uint8_t titleMode_ = 0xff;
  bool titleShown_ = false;
  uint16_t titleTop_[13] = {};  // the TOP- line, moved a row down for room under CO-OP GAME
  uint16_t titleTree_[4][2] = {};  // (The Lost Levels: the small tree beside it, moved right)
  bool hudDrawn_ = false;          // the status bar's name made MARIO & LUIGI
  int heldNeed_[2] = {}, heldHave_[2] = {};  // (testing: see coopLine)
  uint16_t hudName_[5] = {}, hudAmp_ = 0;  // (its own name; the &'s tile)
  bool titleExec(uint32_t pc);
  bool lostLevels_ = false;  // The Lost Levels' engine (each character his own physics and sprites)
  bool liveLuigiShown_ = false;
  uint8_t pad1_ = 0, pad2_ = 0;  // this frame's controllers (SavedJoypad bits)
  uint16_t coopRegs_[3] = {};
  uint8_t hitHold_[8] = {}, palHold_[32] = {};
  uint8_t p2Buttons_[3] = {}, buttonHold_[3] = {};  // Luigi's [A_B, Up_Down, Left_Right_Buttons] of his turn
  int p2Box_[3] = {}, boxHold_[3] = {};  // boxL_/boxR_/boxFrame_[0] of the player not in play
  bool p2Valid_ = false, p2Drawn_ = false, p2Shown_ = false;
  // Luigi's animation frame's tiles (see coopExec)
  uint16_t p2Tiles_[256] = {};
  uint16_t p2TileBase_ = 0, p2TileWords_ = 0;
  bool lockForced_ = false;  // [ScrollLock] set for Mario's turn (Luigi is ahead)
  std::vector<uint8_t> p2_, p1Hold_, keepHold_;
  void coopInit(uint8_t bankLo, uint8_t bankHi);
  bool coopExec(uint32_t pc);
  void p2Sync();
  void swapPlayers(std::vector<uint8_t>& hold);
  void restorePlayer1(std::vector<uint8_t>& hold);
  int p2X() const;
  uint8_t p2Var(uint16_t a) const;  // (one of the kept-aside player's variables)

  // Signature search; bytes equal to 0x100 in `sig` match anything.
  uint32_t findSig(const uint16_t* sig, size_t n, uint8_t bankLo, uint8_t bankHi) const;
  uint32_t findSig(const uint8_t* sig, size_t n, uint8_t bankLo, uint8_t bankHi) const;
  void patch(uint32_t adr, uint8_t v);
  void unpatchAll();
  uint8_t ram(uint16_t a) const { return snes_.wram[a]; }
  // (Pages from $80 up are left of page 0: LL 8-4's maze loop puts the
  // camera on page $FF.)
  int camera() const { return (int8_t)ram(0x071a) * 256 + ram(0x071c); }
  bool leavesBuilt(int x);
  int parserPos() const { return (int8_t)ram(0x0725) * 256 + ram(0x0726) * 16; }
  void trackParser();
};
