// Two-player co-op for Super Mario Bros. (experimental).
//
// The game only ever has one player: Luigi of a 2-player game takes turns in
// Mario's place, in the same variables. Here Luigi plays at the same time,
// on the second controller, by running the game's own player routines a
// second time each frame with his variables swapped in:
//  * GameCoreRoutine calls GameRoutines (the player's routine for the task in
//    hand: under control, PlayerCtrlRoutine: movement, jumping, the screen's
//    scrolling, collision with the level). After Mario's call returns, the
//    CPU is sent back to the call for Luigi (see coopExec), then on.
//  * The same for the player's drawing (GetPlayerOffscreenBits,
//    RelativePlayerPosition, PlayerGfxHandler), Luigi's sprites in OAM
//    entries of their own, in his colours and with his own animation frame
//    (the player's tiles are streamed to VRAM each frame: Mario's; Luigi's
//    are taken from ROM and drawn by the PPU for his sprites; see coopLine).
// The world (enemies, blocks, coins, the screen) is shared. Whoever is
// further right moves the screen; the other is held at its left edge (the
// game's own ChkPOffscr). In this first test Luigi doesn't meet enemies, and
// waits while Mario changes size, goes down a pipe or reaches the flag; he
// comes back beside Mario after that, or if he falls down a hole.
#include <algorithm>
#include "game/smb1.h"
#include "game/palette.h"

namespace {
// GameCoreRoutine (the same in both games from here): JSR GameRoutines;
// LDA OperMode_Task; CMP #3; BCS GameEngine; RTS; GameEngine: JSR
// ProcFireball_Bubble; LDX #0; STX ObjectOffset
const uint16_t kSigCore[] = {0x20, 0x100, 0x100, 0xAD, 0x72, 0x07, 0xC9, 0x03, 0xB0, 0x100,
                             0x60, 0x20, 0x100, 0x100, 0xA2, 0x00, 0x86, 0x9E};
// The enemy loop's end: INX; CPX #$0A; BNE ProcELoop; (then JSL ...)
const uint16_t kSigEnemyLoop[] = {0xE8, 0xE0, 0x0A, 0xD0, 0x100, 0x22};
// PlayerEnemyCollision: LDA FrameCounter; LSR; BCS; JSR CheckPlayerVertical; BCS; CPX #9
const uint16_t kSigPEC[] = {0xA5, 0x09, 0x4A, 0xB0, 0x100, 0x20, 0x100, 0x100, 0xB0, 0x100, 0xE0, 0x09};
// The player drawn across the screen's left edge: X bit 8 for the left column
// (LDA $05; CMP #$F8; BCC; LDA #1; STA $0CD0; STA $0CD8; STA $0CE0; STA $0CE8),
// into Mario's sprites' slots whoever is drawn.
const uint16_t kSigLeftHi[] = {0xA5, 0x05, 0xC9, 0xF8, 0x90, 0x100, 0xA9, 0x01, 0x8D, 0xD0, 0x0C, 0x8D, 0xD8, 0x0C,
                               0x8D, 0xE0, 0x0C, 0x8D, 0xE8, 0x0C, 0x60};
// The OAM high table packed from the per-sprite bytes at $0C00, at the end of
// the frame's drawing: PHD; LDA #$0C; XBA; LDA #$00; TCD; REP #$10
const uint16_t kSigPack[] = {0x0B, 0xA9, 0x0C, 0xEB, 0xA9, 0x00, 0x5B, 0xC2, 0x10};
// JSR GetPlayerOffscreenBits; JSR RelativePlayerPosition; JSR PlayerGfxHandler;
// LDX #1; STX ObjectOffset
const uint16_t kSigDraw[] = {0x20, 0x100, 0x100, 0x20, 0x100, 0x100, 0x20, 0x100, 0x100, 0xA2, 0x01, 0x86, 0x9E};
// PlayerHammerCollision, from its middle: TXA; ASL; ASL; CLC; ADC #$34; TAY;
// JSR PlayerCollisionCore; LDX ObjectOffset; BCC; LDA Misc_Collision_Flag,X
const uint16_t kSigHammer[] = {0x8A, 0x0A, 0x0A, 0x18, 0x69, 0x34, 0xA8, 0x20, 0x100, 0x100,
                               0xA6, 0x9E, 0x90, 0x100, 0xBD};
// The star's end (GameEngine, after the player's drawing): LDA Player_Y_HighPos;
// CMP #2; BPL; LDA StarInvincibleTimer; BEQ; CMP #4; BNE; LDA
// IntervalTimerControl; BNE; JSL (the area's music back)
const uint16_t kSigStarEnd[] = {0xA5, 0xBB, 0xC9, 0x02, 0x10, 0x100, 0xAD, 0xAF, 0x07, 0xF0, 0x100,
                                0xC9, 0x04, 0xD0, 0x100, 0xAD, 0x87, 0x07, 0xD0, 0x100, 0x22};
// The title's choice (after its lock test): LDA $0FF6; ORA $0FF7; BIT #$20;
// BEQ; LDA NumberOfPlayers; EOR #1; BRA; AND #$0C; BEQ; EOR #8; LSR x3;
// CMP NumberOfPlayers; BEQ; STA NumberOfPlayers; LDA #1; STA $1603; JSR
// (the cursor's tiles), at +$27; back at +$2A.
const uint16_t kSigTitleInput[] = {0xAD, 0xF6, 0x0F, 0x0D, 0xF7, 0x0F, 0x89, 0x20, 0xF0, 0x100, 0xAD, 0x7A, 0x07,
                                   0x49, 0x01, 0x80, 0x100, 0x29, 0x0C, 0xF0, 0x100, 0x49, 0x08, 0x4A, 0x4A, 0x4A};
// ... The Lost Levels' (MARIO GAME / LUIGI GAME): the first controller only,
// the cursor's JSL at +$24, back at +$28.
const uint16_t kSigTitleInputLl[] = {0xAD, 0xF6, 0x0F, 0x89, 0x20, 0xF0, 0x100, 0xAD, 0x7A, 0x07, 0x49, 0x01,
                                     0x80, 0x100, 0x29, 0x0C, 0xF0, 0x100, 0x49, 0x08, 0x4A, 0x4A, 0x4A};
// (its start: LDA FrameCounter; LSR; BCC ExPHC)
const uint16_t kSigHammerStart[] = {0xA5, 0x09, 0x4A, 0x90};

// The player's own variables (Super Mario Bros.' addresses in All-Stars;
// NES names in brackets), found from what PlayerCtrlRoutine and the drawing
// write: object slot 0 of the object tables and the player's scalars.
const uint16_t kVars[] = {
    0x000D,                                  // [PreviousA_B_Buttons]
    0x0028, 0x0046, 0x005D, 0x0078,          // [Player_State, _MovingDir, _X_Speed, _PageLoc]
    0x00A0, 0x00BB, 0x00E4, 0x00E5, 0x00EB,  // [Player_Y_Speed, _Y_HighPos], collision state
    0x0202, 0x0219, 0x0237, 0x0256, 0x0284,  // [PlayerFacingDir, Player_X/Y_Position, _SprAttrib]
    0x03AD, 0x03B8, 0x03C1, 0x03D0,          // [Player_Rel_X/YPos, .., _OffscreenBits]
    0x0401, 0x041C, 0x043C,                  // [Player_X_MoveForce (slot), _YMF_Dummy, _Y_MoveForce]
    0x045D, 0x0463, 0x0480, 0x048F,          // [MaximumLeft/RightSpeed], .., [Player_BoundBoxCtrl]
    0x06D5, 0x06FF,                          // [PlayerGfxOffset, Player_X_Scroll]
    0x0700, 0x0701, 0x0702, 0x0703,          // [Player_XSpeedAbsolute, FrictionAdder, RunningSpeed]
    0x0705, 0x0706, 0x0707, 0x0708,          // [Player_X_MoveForce, DiffToHaltJump, JumpOrigin]
    0x0709, 0x070A, 0x070B, 0x070C,          // [VerticalForce(Down), PlayerChangeSizeFlag, AnimTimerSet]
    0x070D, 0x070E, 0x0711, 0x0714,          // [PlayerAnimCtrl, JumpspringAnimCtrl, FireballThrowingTimer, CrouchingFlag]
    0x0754, 0x0755, 0x0756,                  // [PlayerSize, Player_Pos_ForScroll, PlayerStatus]
    0x0789, 0x078A, 0x078B, 0x078C,          // timers [PlayerAnim, JumpSwim, Running, BlockBounce]
    0x078D, 0x078E, 0x0791,                  // [SideCollision, Jumpspring, ClimbSide]
    0x07AE, 0x07AF,                          // [InjuryTimer, StarInvincibleTimer]
    0x0B45,                                  // [Player_SprDataOffset]
    0x06DB,                                  // [JumpspringForce]
    // which character (The Lost Levels: Luigi's physics and his own sprites,
    // [CurrentPlayer] and his graphics' flag; see p2Sync)
    0x0753, 0x0EC2,
    // the animation frame's tiles, streamed to VRAM at the next NMI (flag,
    // source, size, destination): Mario's stay the console's, Luigi's are
    // taken from ROM here (see coopExec, coopLine)
    0x028E, 0x028F, 0x0290, 0x0291, 0x0293, 0x0294, 0x0295, 0x0296,
    0x0E00, 0x0E08, 0x0E87, 0x0E88,          // offscreen bits, relative position (16-bit)
    0x0F9C, 0x0F9D, 0x0F9E, 0x0F9F,          // [BoundingBox] (16-bit)
    // Last, so states saved before them still load (see coopLoad):
    // [Whirlpool_Flag] (his swim stroke's, see coopWhirlRet_: Luigi swam
    // with Mario's weak one while Mario was in a whirlpool) and
    // [Platform_X_Scroll] (a lift's push for the screen, used and cleared by
    // the scrolling: Mario's turn took Luigi's, the screen never followed
    // Luigi on a lift and held him at its edge as the lift went on).
    0x027D, 0x03A1,
    // ... [StompTimer] (a stomp chain's points, each one's own), and the skid's
    // dust (All-Stars' own, $05:D7A1, drawn into OAM entries 12-15: Luigi's
    // moved to 8-11, see coopAfter_)
    0x079D, 0x0E40, 0x0E41, 0x0E42, 0x0E43, 0x0E44, 0x0E45, 0x0E46, 0x0E47, 0x0E48, 0x0E49, 0x0E4A, 0x0E4B, 0x0E4C,
    0x0E4D,
};
constexpr int kVarCount = sizeof(kVars) / sizeof(kVars[0]);
// (appended since the states with [JumpspringForce]; the first two before the rest)
constexpr int kVarsAdded = 17, kVarsAddedFirst = 2;
// Decremented every frame by the game (Luigi's, kept aside, here); and the
// slower ones each time [IntervalTimerControl] runs out.
const uint16_t kFrameTimers[] = {0x0789, 0x078A, 0x078B, 0x078C, 0x078D, 0x078E, 0x0791, 0x079D};
const uint16_t kIntervalTimers[] = {0x07AE, 0x07AF};
// Shared state Luigi's turn must leave as it was: the task, the death music
// and its flags (a fall is handled here), lives; the controller bits the rest
// of the frame reads (fireballs) are Mario's.
const uint16_t kKeep[] = {0x000A, 0x000B, 0x000C, 0x000F, 0x0712, 0x0723, 0x075A, 0x07B1, 0x0E67, 0x0FF4, 0x1602};
constexpr int kKeepCount = sizeof(kKeep) / sizeof(kKeep[0]);
// ... and Luigi's meeting enemies: the task, [TimerControl] (the freeze for a
// size change), the music (a death's).
// (KillPlayer also locks the screen, [ScrollLock] and $0E67, for the death.)
const uint16_t kHitKeep[] = {0x000F, 0x0712, 0x0723, 0x0747, 0x07B1, 0x0E67, 0x1602};
constexpr int kHitKeepCount = sizeof(kHitKeep) / sizeof(kHitKeep[0]);
constexpr uint8_t kP2Oam = 0xB0;  // Luigi's sprites: OAM entries 44-51 (Mario's are at $D0)
// The player palettes (OBJ palette 7): Luigi's, and fire Luigi's (after Mario's
// at $04:99FD and fire Mario's at $04:9A3D); the star's three (then the normal).
constexpr uint32_t kLuigiPal = 0x049A1D, kLuigiFire = 0x049A5D, kStarPals = 0x05ED60;
constexpr uint32_t kMarioPal = 0x0499FD, kMarioFire = 0x049A3D;
constexpr uint8_t kP1Oam = 0xD0;  // the game's player's sprites: OAM entries 52-59

int varIndex(uint16_t a) {
  for (int i = 0; i < kVarCount; i++)
    if (kVars[i] == a) return i;
  return -1;
}
}  // namespace

void Smb1Engine::coopInit(uint8_t bankLo, uint8_t bankHi) {
  uint32_t core = findSig(kSigCore, sizeof(kSigCore) / 2, bankLo, bankHi);
  uint32_t draw = findSig(kSigDraw, sizeof(kSigDraw) / 2, bankLo, bankHi);
  // (Both games keep the player's variables in the same places; The Lost
  // Levels is told by its code bank, $0D on.)
  uint32_t loop = findSig(kSigEnemyLoop, sizeof(kSigEnemyLoop) / 2, bankLo, bankHi);
  uint32_t pec = findSig(kSigPEC, sizeof(kSigPEC) / 2, bankLo, bankHi);
  coopOk_ = core && draw && loop && pec && (core >> 16) == (draw >> 16);
  if (!coopOk_) return;
  lostLevels_ = bankLo >= 0x0D;
  coopEnemyAt_ = loop + 5;
  coopPEC_ = pec;
  // The small player's death in the injury routine (STX $5D; PHX; LDA #1;
  // STA $0754; JSL ...: SMB1 $03:DF7C), its end three bytes before it (LDX
  // $9E; RTS): see coopExec.
  static const uint16_t kSigDeathSmall[] = {0x86, 0x5D, 0xDA, 0xA9, 0x01, 0x8D, 0x54, 0x07, 0x22};
  coopDeathSmall_ = 0;
  if (uint32_t d = findSig(kSigDeathSmall, sizeof(kSigDeathSmall) / 2, bankLo, bankHi))
    if (snes_.peek(d - 3) == 0xA6 && snes_.peek(d - 2) == 0x9E && snes_.peek(d - 1) == 0x60) coopDeathSmall_ = d;
  coopPack_ = findSig(kSigPack, sizeof(kSigPack) / 2, bankLo, bankHi);
  uint32_t hi = findSig(kSigLeftHi, sizeof(kSigLeftHi) / 2, bankLo, bankHi);
  coopLeftHi_ = hi ? hi + 6 : 0;
  coopCall_ = core;
  coopAfter_ = core + 3;
  coopDraw_ = draw;
  coopDrawEnd_ = draw + 9;
  // GameEngine's first call, JSR ProcFireball_Bubble (after the RTS that
  // leaves when not in play), and FireballObjCore, called from it for each
  // fireball (LDX #$00; JSR FireballObjCore); see coopExec.
  coopFire_ = coopFbCore_ = 0;
  {
    uint32_t fire = (core & 0xff0000) | snes_.peek(core + 12) | snes_.peek(core + 13) << 8;
    if (snes_.peek(fire + 0x41) == 0xA2 && snes_.peek(fire + 0x42) == 0x00 && snes_.peek(fire + 0x43) == 0x20) {
      coopFire_ = core + 14;
      coopFbCore_ = (core & 0xff0000) | snes_.peek(fire + 0x44) | snes_.peek(fire + 0x45) << 8;
    }
  }
  // [Enemy_CollisionBits]: PlayerEnemyCollision's AND #$FE; STA abs,X (bit 0:
  // touching the player), each player's own (see coopEnemyAt_)
  collBits_ = 0;
  for (uint32_t a = pec; a < pec + 0x60 && !collBits_; a++)
    if (snes_.peek(a) == 0x29 && snes_.peek(a + 1) == 0xFE && snes_.peek(a + 2) == 0x9D)
      collBits_ = (uint16_t)(snes_.peek(a + 3) | snes_.peek(a + 4) << 8);
  // The hammers (see coopHammer_)
  coopHammer_ = coopHammerRts_ = 0;
  if (uint32_t h = findSig(kSigHammer, sizeof(kSigHammer) / 2, bankLo, bankHi)) {
    for (uint32_t a = h - 4; a > h - 0x20 && !coopHammer_; a--)
      if (snes_.peek(a) == 0xA5 && snes_.peek(a + 1) == 0x09 && snes_.peek(a + 2) == 0x4A && snes_.peek(a + 3) == 0x90)
        coopHammer_ = a;
    if (coopHammer_) {
      coopHammerRts_ = coopHammer_ + 5 + (int8_t)snes_.peek(coopHammer_ + 4);  // (BCC ExPHC: its RTS)
      hamFlag_ = (uint16_t)(snes_.peek(h + 15) | snes_.peek(h + 16) << 8);
      if (snes_.peek(coopHammerRts_) != 0x60) coopHammer_ = 0;
    }
  }
  // PlayerEndLevel: LDA #1; JSR AutoControlPlayer; LDA Player_Y_Position;
  // CMP #$AE (the walk he takes into the castle: see coopAfter_)
  static const uint16_t kSigEndLevel[] = {0xA9, 0x01, 0x20, 0x100, 0x100, 0xAD, 0x37, 0x02, 0xC9, 0xAE};
  coopWalk_ = 0;
  if (uint32_t e = findSig(kSigEndLevel, sizeof(kSigEndLevel) / 2, bankLo, bankHi))
    coopWalk_ = (e & 0xff0000) | snes_.peek(e + 3) | snes_.peek(e + 4) << 8;
  // VictoryMode: LDA OperMode_Task; BEQ; LDX #0; STX ObjectOffset; JSR
  // EnemiesAndLoopsCore; JSR RelativePlayerPosition; JMP PlayerGfxHandler
  static const uint16_t kSigVictory[] = {0xAD, 0x72, 0x07, 0xF0, 0x100, 0xA2, 0x00, 0x86, 0x9E,
                                         0x20, 0x100, 0x100, 0x20, 0x100, 0x100, 0x4C};
  // (The thanks' choice: TAY; BNE; LDA CurrentPlayer; BEQ; INY; BNE)
  static const uint16_t kSigThanks[] = {0xA8, 0xD0, 0x100, 0xAD, 0x53, 0x07, 0xF0, 0x100, 0xC8, 0xD0};
  coopThanks_ = 0;
  if (uint32_t t = findSig(kSigThanks, sizeof(kSigThanks) / 2, bankLo, bankHi)) coopThanks_ = t + 3;
  // The ending's "push start" (LDA $0FF6 or LDA $0FF6,X; AND #$10; BEQ; INC $1680)
  static const uint16_t kSigEndStart[] = {0x29, 0x10, 0xF0, 0x100, 0xEE, 0x80, 0x16};
  coopEndStart_ = 0;
  if (uint32_t e = findSig(kSigEndStart, sizeof(kSigEndStart) / 2, bankLo, bankHi))
    if (snes_.peek(e - 2) == 0xF6 && snes_.peek(e - 1) == 0x0F) coopEndStart_ = e - 3;
  coopVicDraw_ = 0;
  if (uint32_t v = findSig(kSigVictory, sizeof(kSigVictory) / 2, bankLo, bankHi)) coopVicDraw_ = v + 12;
  // The title's menu (Super Mario Bros.: 1 PLAYER GAME / 2 PLAYER GAME): Select
  // toggles [NumberOfPlayers] $077A, Up/Down pick; then the cursor drawn
  // (JSR, its tiles in the VRAM update buffer at $1700).
  // The lives a game starts with (from the save, LDA $7FFB03; STA
  // NumberofLives): Super Mario Bros. $05:C980, The Lost Levels $0F:D07B.
  livesSet_ = 0;
  for (uint32_t a : {0x05c980u, 0x0fd07bu})
    if (snes_.peek(a - 4) == 0xAF && snes_.peek(a - 3) == 0x03 && snes_.peek(a - 2) == 0xFB && snes_.peek(a) == 0x8D &&
        snes_.peek(a + 1) == 0x5A && snes_.peek(a + 2) == 0x07 && (a == 0x0fd07bu) == lostLevels_)
      livesSet_ = a + 3;
  titleInput_ = 0;
  if (uint32_t t = lostLevels_ ? findSig(kSigTitleInputLl, sizeof(kSigTitleInputLl) / 2, bankLo, bankHi)
                               : findSig(kSigTitleInput, sizeof(kSigTitleInput) / 2, bankLo, bankHi))
    titleInput_ = t;
  // ProcessWhirlpools (LDA AreaType; BNE; STA Whirlpool_Flag; JSL (All-Stars'
  // water palette); LDA TimerControl; BNE; LDY #4: the whirlpools, each
  // pulling the game's player in), called once from GameEngine: its loop and
  // the JSR's return (see coopWhirlRet_).
  static const uint16_t kSigWhirl[] = {0xA5, 0x5C, 0xD0, 0x100, 0x8D, 0x7D, 0x02, 0x22, 0x100,
                                       0x100, 0x100, 0xAD, 0x47, 0x07, 0xD0, 0x100, 0xA0, 0x04};
  coopWhirl_ = coopWhirlRet_ = 0;
  if (uint32_t wp = findSig(kSigWhirl, sizeof(kSigWhirl) / 2, bankLo, bankHi)) {
    const uint32_t bank = wp & 0xff0000;
    for (uint32_t a = bank | 0x8000; a < (bank | 0xfffd) && !coopWhirlRet_; a++)
      if (snes_.peek(a) == 0x20 && snes_.peek(a + 1) == (wp & 0xff) && snes_.peek(a + 2) == ((wp >> 8) & 0xff))
        coopWhirlRet_ = a + 3;
    if (coopWhirlRet_) coopWhirl_ = wp + 16;
  }
  // The air bubbles (BubbleCheck: LDA Bubble_Y_Position,X; CMP #$F8; BNE
  // MoveBubl; LDA AirBubbleTimer; BNE; SetupBubble: LDY #0; LDA
  // PlayerFacingDir; ...): the game's player's only. Its timer's table
  // (LDY $07; LDA BubbleTimerData,Y; STA AirBubbleTimer), for Luigi's (see coopBubble).
  static const uint16_t kSigBubble[] = {0xBD, 0x51, 0x02, 0xC9, 0xF8, 0xD0, 0x100, 0xAD, 0x9E, 0x07, 0xD0, 0x100,
                                        0xA0, 0x00, 0xAD, 0x02, 0x02, 0x4A, 0x90, 0x02, 0xA0, 0x08};
  bubbleTimes_ = 0;
  if (uint32_t b = findSig(kSigBubble, sizeof(kSigBubble) / 2, bankLo, bankHi))
    for (uint32_t a = b + 22; a < b + 60 && !bubbleTimes_; a++)
      if (snes_.peek(a) == 0xA4 && snes_.peek(a + 1) == 0x07 && snes_.peek(a + 2) == 0xB9 && snes_.peek(a + 5) == 0x8D)
        bubbleTimes_ = (b & 0xff0000) | snes_.peek(a + 3) | snes_.peek(a + 4) << 8;
  // A castle's loop (ProcLoopCommand's test: LDA Player_Y_Position; CMP
  // LoopCmdYPosition,Y; BNE; LDA Player_State; CMP #0; BNE: the right way's
  // height, on the ground, or the area loops back): the game's player's only,
  // as the level is read where the one in front is. Passed for either.
  static const uint16_t kSigLoop[] = {0xAD, 0x37, 0x02, 0xD9, 0x100, 0x100, 0xD0, 0x100, 0xA5, 0x28, 0xC9, 0x00, 0xD0};
  coopLoop_ = findSig(kSigLoop, sizeof(kSigLoop) / 2, bankLo, bankHi);
  // The menus' reads of the first controller (see coopExec): Super Mario
  // Bros.' after the ending (LDA $0FF6,X; STA $F2; AND #$2C), The Lost
  // Levels' after the ending (LDA $0FF6; STA $F2; AND #$2C) and after a game
  // over (LDA $0FF6; AND #$0C; BEQ).
  static const uint16_t kSigMenuA[] = {0xBD, 0xF6, 0x0F, 0x85, 0xF2, 0x29, 0x2C};
  static const uint16_t kSigMenuB[] = {0xAD, 0xF6, 0x0F, 0x85, 0xF2, 0x29, 0x2C};
  static const uint16_t kSigMenuC[] = {0xAD, 0xF6, 0x0F, 0x29, 0x0C, 0xF0};
  coopMenu_[0] = findSig(kSigMenuA, sizeof(kSigMenuA) / 2, bankLo, bankHi);
  coopMenu_[1] = findSig(kSigMenuB, sizeof(kSigMenuB) / 2, bankLo, bankHi);
  coopMenu_[2] = findSig(kSigMenuC, sizeof(kSigMenuC) / 2, bankLo, bankHi);
  coopStar_ = coopStarMusic_ = 0;
  if (uint32_t a = findSig(kSigStarEnd, sizeof(kSigStarEnd) / 2, bankLo, bankHi)) {
    coopStar_ = a + 6;
    coopStarMusic_ = snes_.peek(a + 21) | snes_.peek(a + 22) << 8 | snes_.peek(a + 23) << 16;
  }
  objBefore_.assign(0x2000, 0);
  objAfter_.assign(0x2000, 0);
  p2_.assign(kVarCount, 0);
  p1Hold_.assign(kVarCount, 0);
  keepHold_.assign(kKeepCount, 0);
}

void Smb1Engine::setCoop(bool on) {
  // (Not in Super Mario Bros.' own 2-player game ([NumberOfPlayers] 1),
  // Luigi's turns: the two together, the level couldn't be finished.)
  if (on && ram(0x077A) != 0) on = false;
  if (on == coop_) return;
  coop_ = on;
  p2Valid_ = p2Drawn_ = p2Known_ = false;
  deathTune_ = false;
  musicBack_ = 0;
  luigiLive_ = liveCtrl_ = false;
  p2Wait_ = p2Out_ = false;
}

void Smb1Engine::coopPoke(uint16_t a, uint8_t v) {
  int i = varIndex(a);
  if (i >= 0 && !p2_.empty()) p2_[i] = v;
}

uint8_t Smb1Engine::p2Var(uint16_t a) const {
  const int i = varIndex(a);
  return i >= 0 && !p2_.empty() ? p2_[i] : 0;
}

int Smb1Engine::p2X() const { return p2_[varIndex(0x0078)] * 256 + p2_[varIndex(0x0219)]; }

// Luigi beside Mario: Mario's variables, 16 px behind (where the screen
// allows); his own power-up kept once he has one (after a pipe he was Mario's
// size).
void Smb1Engine::p2Sync() {
  if (!p2Known_ && lostLevels_) luigiLive_ = ram(0x0753) != 0;  // (a Luigi game: Mario joins him)
  if (!p2Known_ && !lostLevels_ && luigiGame_) {
    // (Super Mario Bros.' 1P LUIGI GAME: Mario joins him. Co-op's way from here: the game's player
    // its Mario ([CurrentPlayer] 0, his controller the first), Luigi shown and moved as co-op shows
    // and moves him (see luigiFrame), Mario the one kept aside on the second controller.)
    luigiLive_ = true;
    snes_.wram[0x0753] = 0;
  }
  const uint8_t size = p2_[varIndex(0x0754)], status = p2_[varIndex(0x0756)], star = p2_[varIndex(0x07AF)];
  for (int i = 0; i < kVarCount; i++) p2_[i] = snes_.wram[kVars[i]];
  // (his own power-up and star, not the other's: a star was handed on)
  p2_[varIndex(0x07AF)] = p2Known_ ? star : 0;
  if (p2Known_) {
    p2_[varIndex(0x0754)] = size;
    p2_[varIndex(0x0756)] = status;
  }
  p2Known_ = true;
  // (behind him, into the left margin as far as players may go; at its edge,
  // ahead of him: on him, he was hidden. Where he stands on the level, only
  // with ground there too, else on him: he came back over a pit's edge, to
  // fall straight in.)
  const int x0 = ram(0x0078) * 256 + ram(0x0219), y = ram(0x0237);
  int x = x0 - 16;
  if (x < playerLeftLimit()) x += 32;
  auto mt = [&](int px, int py) { return metatile(px, py); };
  if (ram(0x0028) != 0 && ram(0x0237) >= 0x40) {
    // (in the air, a lift going down: where he is, or beside him where
    // that's clear and comes down on the same ground (up a vine, out of a
    // castle's loop: one hid the other); not dropping into an area)
    auto ground = [&](int px) {
      for (int py = y + 24; py < 0x20 + 13 * 16; py += 16)
        if (mt(px + 8, py) != 0) return mt(px + 8, py) > 0 ? py >> 4 : -1;
      return -1;
    };
    auto clear = [&](int px) {
      return px >= playerLeftLimit() && mt(px + 8, y + 8) == 0 && mt(px + 8, y + 24) == 0 && ground(px) >= 0 &&
             ground(px) == ground(x0);
    };
    x = clear(x0 - 16) ? x0 - 16 : clear(x0 + 16) ? x0 + 16 : x0;
  }
  if (ram(0x00BB) == 1 && ram(0x0028) == 0) {
    auto room = [&](int px) {
      return px >= playerLeftLimit() && mt(px + 8, y + 36) > 0 && mt(px + 8, y + 24) <= 0 && mt(px + 8, y + 8) <= 0;
    };
    if (mt(x0 + 8, y + 36) <= 0) x = x0;  // (on a lift)
    else if (room(x0 - 16)) x = x0 - 16;
    else if (room(x0 + 16)) x = x0 + 16;
    else x = x0;
  }
  p2Box_[0] = boxL_[0] + (x - x0);
  p2Box_[1] = boxR_[0] + (x - x0);
  p2_[varIndex(0x0219)] = x & 0xff;
  p2_[varIndex(0x0078)] = (x >> 8) & 0xff;
  p2_[varIndex(0x0B45)] = kP2Oam;
  // (The Lost Levels: the one kept aside is the other character, his own
  // physics and sprites; in Super Mario Bros. both are the game's Mario but
  // for colours (see coopLine). The game's player says which he is.)
  if (lostLevels_) {
    const bool luigi = !luigiLive_;
    p2_[varIndex(0x0753)] = luigi;
    p2_[varIndex(0x0EC2)] = luigi;
  } else {
    // (Super Mario Bros.: Luigi's own sprites (The Lost Levels', which the game has behind $0EC2) for
    // whichever of the two is Luigi, not Mario recoloured)
    p2_[varIndex(0x0EC2)] = !luigiLive_;
  }
  p2Box_[2] = (int)boxFrame_[0];
  p2Valid_ = true;
  for (int k = 0; k < 10; k++) {
    p2Plat_[k] = 0xff;
    p2Coll_[k] = 0;
  }
  std::fill(std::begin(p2Ham_), std::end(p2Ham_), 0);
}

// (With the variables, the player's box in level coordinates the widescreen
// collision tests keep (see onExec's boundBox_): Mario's in the left margin
// was tested against Luigi's otherwise, and its 8-bit wrap hit enemies at the
// screen's right edge.)
void Smb1Engine::swapPlayers(std::vector<uint8_t>& hold) {
  for (int i = 0; i < kVarCount; i++) {
    hold[i] = snes_.wram[kVars[i]];
    snes_.wram[kVars[i]] = p2_[i];
  }
  boxHold_[0] = boxL_[0];
  boxHold_[1] = boxR_[0];
  boxHold_[2] = (int)boxFrame_[0];
  boxL_[0] = p2Box_[0];
  boxR_[0] = p2Box_[1];
  boxFrame_[0] = (uint32_t)p2Box_[2];
}

void Smb1Engine::restorePlayer1(std::vector<uint8_t>& hold) {
  for (int i = 0; i < kVarCount; i++) {
    p2_[i] = snes_.wram[kVars[i]];
    snes_.wram[kVars[i]] = hold[i];
  }
  p2Box_[0] = boxL_[0];
  p2Box_[1] = boxR_[0];
  p2Box_[2] = (int)boxFrame_[0];
  boxL_[0] = boxHold_[0];
  boxR_[0] = boxHold_[1];
  boxFrame_[0] = (uint32_t)boxHold_[2];
}

// Luigi's air bubbles, as the game's (SetupBubble) for its player: in a
// water area, his own timer run out, into a free one of the three bubbles
// (Y $F8), from his mouth (facing right 9 px on), the timer again from the
// game's table by the bubble's random bit.
void Smb1Engine::coopBubble() {
  uint8_t* w = snes_.wram;
  if (p2Bubble_) p2Bubble_--;
  if (!bubbleTimes_ || !coop_ || !p2Valid_ || p2Out_ || p2Wait_ || w[0x000F] != 8 || w[0x005C] != 0 || w[0x0747] ||
      p2_[varIndex(0x00BB)] != 1 || p2Bubble_)
    return;
  for (int k = 0; k < 3; k++) {
    if (w[0x0251 + k] != 0xF8) continue;
    const int off = (p2_[varIndex(0x0202)] & 1) ? 9 : 0;
    const int x = p2_[varIndex(0x0219)] + off;
    w[0x0233 + k] = (uint8_t)x;
    w[0x0092 + k] = (uint8_t)(p2_[varIndex(0x0078)] + (x >> 8));
    w[0x0251 + k] = (uint8_t)(p2_[varIndex(0x0237)] + 8);
    w[0x00D5 + k] = 1;
    p2Bubble_ = snes_.peek(bubbleTimes_ + (w[0x07B8 + k] & 1));
    return;
  }
}

// The collision map's metatile at a level position: 0 is air; -1 not known.
int Smb1Engine::metatile(int px, int py) const {
  if (px < 0 || px + 8 > parserPos() || py < 0x20 || py >= 0x20 + 13 * 16) return -1;
  const int col = px >> 4, row = (py - 0x20) >> 4;
  return (int)ram((((col >> 4) & 1) ? 0x05d0 : 0x0500) + row * 16 + (col & 15));
}

// The other player in play and on his feet: someone to come back beside
// (with a life for it; or, for the game's player's death, without: on the
// last life the other plays on and the one who died sits out, see p2Lose,
// whichever of them it was; the game's player's death was the game over).
bool Smb1Engine::p2Alive(bool needLife) const {
  // (not one already below the floor, falling into a pit: he'd play on for a moment)
  const bool sinking = p2_[varIndex(0x00BB)] == 1 && p2_[varIndex(0x0237)] >= 0xD0;
  return coop_ && p2Valid_ && !p2Out_ && !p2Wait_ && (int8_t)p2_[varIndex(0x00BB)] < 2 && !sinking &&
         (!needLife || snes_.wram[0x075A] > 0);
}

// The cheats for the other player as for the game's (the star, no damage,
// fire, the moon jump): they had followed whichever was the game's player.
void Smb1Engine::coopCheats(uint32_t f) {
  if (!coop_ || !p2Valid_ || p2Out_ || p2_.empty() || coopMidTurn()) return;
  if ((f & kCheatStar) && p2_[varIndex(0x07AF)] < 0x10) p2_[varIndex(0x07AF)] = 0x10;
  if ((f & kCheatNoDamage) && p2_[varIndex(0x07AE)] < 2) p2_[varIndex(0x07AE)] = 2;
  if (f & kCheatFire) {
    p2_[varIndex(0x0756)] = 2;
    p2_[varIndex(0x0754)] = 0;
  }
  if ((f & kCheatMoonJump) && (snes_.input[liveCtrl_ ? 0 : 1] & kPadB) && (int8_t)p2_[varIndex(0x00A0)] > 1)
    p2_[varIndex(0x00A0)] = 1;
}

// One left idle (3 s) at the screen's left edge, the other well on: brought
// to the other, where he stands. (The edge pushed him on with the screen,
// through pipes and walls and into pits, a life each.)
void Smb1Engine::coopBringIdle() {
  uint8_t* w = snes_.wram;
  if (!coop_ || !p2Valid_ || p2Out_ || p2Wait_ || w[0x000F] != 8 || w[0x0770] != 1 || p2_.empty()) return;
  const int left = playerLeftLimit();
  const int gx = w[0x0078] * 256 + w[0x0219], px = p2X();
  const int gPad = liveCtrl_ ? 1 : 0, pPad = 1 - gPad;
  // (each on his feet: Player_State 0, on the screen; or swimming, see coopStands)
  const bool gStands = coopStands(false), pStands = coopStands(true);
  // (Where each last stood on the floor: one in the air or swimming brings
  // the other there, so the screen didn't wait at its edge through each jump.
  // Kept while that spot is still well ahead on the screen.)
  static const uint16_t kAt[5] = {0x0078, 0x0219, 0x0237, 0x00BB, 0x0028};
  auto onFloor = [&](bool p2) {
    auto v = [&](uint16_t a) { return p2 ? p2_[varIndex(a)] : w[a]; };
    return v(0x00BB) == 1 && v(0x0028) == 0;
  };
  const int area = w[0x0750] << 16 | w[0x0760] << 8 | w[0x075F];  // (area pointer, area, world)
  if (stoodArea_ != area) {
    stoodArea_ = area;
    stoodOk_[0] = stoodOk_[1] = false;
  }
  for (int k = 0; k < 2; k++)
    if (onFloor(k == 1)) {
      for (int i = 0; i < 5; i++) stoodAt_[k][i] = k ? p2_[varIndex(kAt[i])] : w[kAt[i]];
      stoodOk_[k] = true;
    }
  // Where to bring one left idle, by the other (k): where the other last
  // stood on the floor, still on the screen ahead of the edge (at least
  // `from` px in); else, swimming, where he is now.
  auto target = [&](int k, int from, uint8_t* to) {
    const int x = stoodAt_[k][0] * 256 + stoodAt_[k][1];
    if (stoodOk_[k] && x >= left + from) {
      std::copy(stoodAt_[k], stoodAt_[k] + 5, to);
      return true;
    }
    if (!(k ? pStands : gStands)) return false;
    for (int i = 0; i < 5; i++) to[i] = k ? p2_[varIndex(kAt[i])] : w[kAt[i]];
    return true;
  };
  // (Falling into a hole: below the floor's level, not on it. One left idle
  // there is brought back instead: sinking in water by a swimmer, pushed on
  // by the screen, he lost a life each time.)
  auto falling = [&](bool p2) {
    auto v = [&](uint16_t a) { return p2 ? p2_[varIndex(a)] : w[a]; };
    return v(0x00BB) == 1 && v(0x0237) >= 0xD0 && v(0x0028) != 0;
  };
  uint8_t to[5];
  const bool pIdle = idle_[pPad] >= 180 && idle_[gPad] < 180, gIdle = idle_[gPad] >= 180 && idle_[pPad] < 180;
  if (idle_[pPad] >= 180 && ((px <= left + 4 && gx > px + 32 && target(0, 24, to)) ||
                             (pIdle && falling(true) && target(0, 8, to)))) {
    for (int i = 0; i < 5; i++) p2_[varIndex(kAt[i])] = to[i];
    for (uint16_t a : {0x005D, 0x00A0, 0x0700, 0x0705}) p2_[varIndex(a)] = 0;
    p2Box_[2] = 0;  // (his box from his new place: see swapPlayers)
  } else if (idle_[gPad] >= 180 && ((gx <= left + 4 && px > gx + 32 && target(1, 24, to)) ||
                                    (gIdle && falling(false) && target(1, 8, to)))) {
    for (int i = 0; i < 5; i++) w[kAt[i]] = to[i];
    for (uint16_t a : {0x005D, 0x00A0, 0x0700, 0x0705}) w[a] = 0;
    boxFrame_[0] = 0;
  } else if ((pIdle && falling(true)) || (gIdle && falling(false) && p2Alive(false))) {
    // Nowhere to bring him (the other high in the air, his last floor off the
    // screen: The Lost Levels' springs): he rests out of play, no life lost,
    // and comes back beside the other once he stands (see p2Return). The game's
    // player hands over to the other first.
    if (gIdle) swapRoles();
    p2Valid_ = false;
    p2Wait_ = true;
    restWait_ = true;
  }
}

// One left standing (his controller untouched for 3 s) while the other plays
// on can't be hurt till he moves: he couldn't dodge (a Goomba walked into
// him). His injury timer ([InjuryTimer] $07AE, what a hit's grace runs on,
// every 21 frames) kept at 2, without the blink (see blinkLda_); back to 0
// when he moves, unless a real grace is still running past it.
void Smb1Engine::coopIdleSafe() {
  uint8_t* w = snes_.wram;
  const int gPad = liveCtrl_ ? 1 : 0, pPad = 1 - gPad;
  const bool play = coop_ && p2Valid_ && !p2Out_ && !p2Wait_ && !p2_.empty() && w[0x000F] == 8 && w[0x0770] == 1;
  for (int k = 0; k < 2; k++) {
    const int pad = k ? pPad : gPad, other = 1 - pad;
    if (k && p2_.empty()) {
      idleSafe_[pad] = false;
      continue;
    }
    uint8_t& t = k ? p2_[varIndex(0x07AE)] : w[0x07AE];
    if (play && still_[pad] >= 180 && still_[other] < 180) {
      if (t < 2) t = 2;
      idleSafe_[pad] = true;
    } else if (idleSafe_[pad]) {
      if (t <= 2) t = 0;
      idleSafe_[pad] = false;
    }
  }
}

// In the middle of a turn of the other player's (his variables in, the
// game's player's kept aside): a frame the game overran ends there. Not a
// moment for a save state or rewind's snapshot (see the app).
bool Smb1Engine::coopMidTurn() const {
  return coopMove_ || coopDrawPhase_ || coopWhirlPhase_ || coopFirePhase_ || coopObjPhase_ || coopHamPhase_ || vicPhase_ ||
         coopEnemy_ >= 0 || lockForced_;  // (... or the game's player's turn with the screen's lock set for it)
}

// Somewhere to come back: the game's player on his feet (on the ground or a
// lift), or swimming. (Brought back as he fell, over the same pit, both were
// lost there one after the other, a life each frame.)
bool Smb1Engine::p2CanReturn() const {
  bool lift = false;  // (on a lift: [PlatformCollisionFlag] his)
  for (int k = 0; k < 10 && !lift; k++) {
    const uint8_t id = ram(0x1C + k), f = ram(0x03A2 + k);
    if (ram(0x10 + k) && id >= 0x24 && id <= 0x2C) lift = id >= 0x2B ? f != 0 && f != 0xff : !(f & 0x80);
  }
  return ram(0x00BB) == 1 && (ram(0x0028) == 0 || ram(0x005C) == 0 || lift);
}

// The two players change places: the one kept aside becomes the game's
// player and the game's player is kept aside (each OAM slot stays its own).
// Either character can be either: whoever takes a pipe, a vine or the flag,
// or carries on after the other's death, is the game's player from then on.
void Smb1Engine::swapRoles() {
  uint8_t* w = snes_.wram;
  const int o = varIndex(0x0B45);
  const uint8_t oamLive = w[0x0B45], oamOther = p2_[o];
  for (int i = 0; i < kVarCount; i++) std::swap(w[kVars[i]], p2_[i]);
  w[0x0B45] = oamLive;
  p2_[o] = oamOther;
  std::swap(boxL_[0], p2Box_[0]);
  std::swap(boxR_[0], p2Box_[1]);
  int f = (int)boxFrame_[0];
  boxFrame_[0] = (uint32_t)p2Box_[2];
  p2Box_[2] = f;
  luigiLive_ = !luigiLive_;
  liveCtrl_ = !liveCtrl_;
  lastP2Inj_ = p2_[varIndex(0x07AE)];  // (the other's, not a hit)
  // (Each enemy's "touching the player" bit is the game's player's, the
  // other's kept aside (see coopEnemyAt_): swapped with the roles. Kept, the
  // one taking over had the other's touch: small Mario killed by a Koopa,
  // Luigi walked through it as if already touching it, ten frames.)
  if (collBits_)
    for (int k = 0; k < 10; k++) {
      uint8_t& b = w[collBits_ + k];
      const uint8_t mine = b & 1;
      b = (uint8_t)((b & ~1) | (p2Coll_[k] & 1));
      p2Coll_[k] = mine;
    }
  // (The game's own box for its player ($0F9C) is still the other's till his
  // update works it out again: until then nothing meets him (see collide_).
  // Mario fallen into a castle's lava: a Podoboo by Mario's box in the lava
  // killed Luigi, the game's player now, and the level started over.)
  boxAfterSwap_ = true;
}

// The player kept aside lost a life (an enemy, a hole): lives are shared,
// one each death; he comes back beside the other once he's on his feet
// (p2Return). With none left he sits out until the next area or try.
void Smb1Engine::p2Lose() {
  uint8_t* w = snes_.wram;
  p2Valid_ = false;
  p2Hurts_++;
  deathTune_ = true;
  if (w[0x075A] > 0) {
    w[0x075A]--;
    p2Wait_ = true;
  } else {
    p2Out_ = true;
  }
}

void Smb1Engine::p2Return() {
  p2Wait_ = false;
  p2Sync();
  for (uint16_t a : {0x005D, 0x0700, 0x0705})  // (standing: the other's run not handed on, he slid)
    p2_[varIndex(a)] = 0;
  if (restWait_) {  // (an idle brother kept out of a hole (see coopBringIdle): as he was)
    restWait_ = false;
    return;
  }
  p2_[varIndex(0x0754)] = 1;     // small
  p2_[varIndex(0x0756)] = 0;
  p2_[varIndex(0x07AE)] = 0x08;  // blinking a while (as after a hit)
  lastP2Inj_ = 0x08;
}

// The second player, kept in save states and rewind: a tagged block after
// the widescreen state ('C', a version, then the variables kept aside and
// how they stand). States without it (or another list of variables) bring
// him back beside the game's player.
void Smb1Engine::coopSave(std::vector<uint8_t>& out) const {
  if (!coopOk_ || p2_.empty()) return;
  out.push_back('C');
  out.push_back(1);  // (version)
  out.push_back((uint8_t)((int)p2Valid_ | p2Out_ << 1 | p2Known_ << 2 | luigiLive_ << 3 | p2Drawn_ << 4 | liveCtrl_ << 5 |
                           p2Wait_ << 6));
  out.push_back((uint8_t)kVarCount);
  out.insert(out.end(), p2_.begin(), p2_.end());
  for (int v : p2Box_)
    for (int k = 0; k < 4; k++) out.push_back((uint8_t)(v >> (8 * k)));
  out.insert(out.end(), p2Buttons_, p2Buttons_ + 3);
  out.push_back(p2PrevAB_);
  for (uint16_t v : {p2TileBase_, p2TileWords_}) {
    out.push_back((uint8_t)v);
    out.push_back((uint8_t)(v >> 8));
  }
  for (uint16_t v : p2Tiles_) {
    out.push_back((uint8_t)v);
    out.push_back((uint8_t)(v >> 8));
  }
  // (each controller's idle frames: resting, see holdScroll; after the rest, older states lack them)
  for (uint32_t v : idle_)
    for (int k = 0; k < 4; k++) out.push_back((uint8_t)(v >> (8 * k)));
  // (a death's tune and the music after it: the sound chip as it was before the tune, while it plays)
  out.push_back((uint8_t)deathTune_);
  for (int k = 0; k < 4; k++) out.push_back((uint8_t)((uint32_t)musicBack_ >> (8 * k)));
  out.push_back(musicTrack_);
  out.push_back(lastTrack_);
  const uint32_t snap = musicBack_ > 0 ? (uint32_t)apuSnap_.size() : 0;
  for (int k = 0; k < 4; k++) out.push_back((uint8_t)(snap >> (8 * k)));
  out.insert(out.end(), apuSnap_.begin(), apuSnap_.begin() + snap);
}

// The title's third choice, CO-OP GAME, on the free row under the game's two
// (Super Mario Bros.: 1 PLAYER GAME / 2 PLAYER GAME; The Lost Levels: MARIO
// GAME / LUIGI GAME). The game's choice is [NumberOfPlayers] $077A (in The
// Lost Levels, a Luigi game); co-op is its first ($077A 0) with Luigi on
// controller 2, the cursor on the third row. Its input: Select steps through
// the three, Up and Down move (the game's own test replaced); the game's
// cursor drawn after it (its tiles in the VRAM update buffer at $1700), both
// its rows blank for the third. The row is written to the picture's tile map
// while the menu is up, lined up under the others; the TOP- line under it a
// row lower for room (and, in The Lost Levels, the small tree there two tiles
// right). The pick is taken as the game starts ([OperMode] $0770 0 to 1), for
// SmasGame to make co-op on or off.
namespace {
struct TitleLayout {
  uint16_t row1;         // the first choice's first tile (row 18): the tile map word
  uint16_t first, second;  // ... its tile there, and the second choice's
  uint16_t cursor;       // the cursor's column, row 18
  uint16_t text[13];     // the third row's tiles from row1's column (0: end)
  int topFrom, topCount; // the TOP- line's columns (from row1's) to move down
  bool tree;             // a small tree at columns 22-23, rows 22-25, to move right
  int call, after;       // offsets from the input code: its cursor's call, back from it
};
// (the title's font: 0-9, A-Z from $0A, - $28, blank $24)
const TitleLayout kTitleSmb1 = {0x024a, 0x0801, 0x0802, 0x0249,
                                {0x0C, 0x18, 0x28, 0x18, 0x19, 0x24, 0x10, 0x0A, 0x16, 0x0E, 0, 0, 0},
                                0, 13, false, 0x27, 0x2A};  // ("CO-OP GAME": from the others' column)
const TitleLayout kTitleLl = {0x024e, 0x0816, 0x0815, 0x024c,
                              {0x0C, 0x18, 0x28, 0x18, 0x19, 0x24, 0x10, 0x0A, 0x16, 0x0E, 0, 0, 0},
                              -4, 12, true, 0x24, 0x28};  // ("CO-OP GAME": under MARIO GAME)
constexpr uint16_t kTitleBlank = 0x1024, kTitleCursor = 0x122e;
}  // namespace

bool Smb1Engine::titleExec(uint32_t pc) {
  CpuRegs& r = snes_.cpu.r;
  uint8_t* w = snes_.wram;
  if (pc == livesSet_) {
    // (a co-op game, as its start is picked on the title or for a continue:
    // lives for two, at least 10 (the save's count if more))
    if ((titleShown_ ? titleCursor_ == 2 : coop_) && w[0x075A] < 9) w[0x075A] = 9;
    return false;
  }
  const TitleLayout& L = lostLevels_ ? kTitleLl : kTitleSmb1;
  if (pc == titleInput_) {
    const uint8_t pressed = lostLevels_ ? w[0x0FF6] : w[0x0FF6] | w[0x0FF7];
    if (titleCursor_ != 2) titleCursor_ = w[0x077A] ? 1 : 0;
    const int was = titleCursor_;
    if (pressed & 0x20) titleCursor_ = (titleCursor_ + 1) % 3;
    else if (pressed & 0x08) titleCursor_ = std::max(titleCursor_ - 1, 0);
    else if (pressed & 0x04) titleCursor_ = std::min(titleCursor_ + 1, 2);
    if (titleCursor_ != was) {
      w[0x077A] = titleCursor_ == 1;
      w[0x1603] = 0x01;  // (the game's sound for it)
    }
    r.pc = (uint16_t)(titleInput_ + L.call);  // (its cursor)
    return true;
  }
  // (its cursor drawn: on the third row, neither of its own)
  if (titleCursor_ == 2) {
    w[0x1706] = kTitleBlank & 0xff;
    w[0x1707] = kTitleBlank >> 8;
    w[0x170A] = kTitleBlank & 0xff;
    w[0x170B] = kTitleBlank >> 8;
  }
  return false;
}

void Smb1Engine::titleFrame() {
  if (!titleInput_) return;
  const TitleLayout& L = lostLevels_ ? kTitleLl : kTitleSmb1;
  uint16_t* vram = snes_.ppu.vram;
  const uint8_t mode = ram(0x0770);
  // (1P MARIO GAME / 1P LUIGI GAME: see smb1luigi.cpp. The game started from it as Luigi: his from its
  // first frame.)
  if (titleMode_ == 0 && mode == 1 && luigiMenu_) {
    luigiGame_ = !lostLevels_ && luigiPick_ && titleCursor_ == 0;
    if (luigiGame_) snes_.wram[0x0753] = snes_.wram[0x0EC2] = 1;
  }
  if (titleMode_ == 0 && mode == 1 && titleShown_) titlePick_ = titleCursor_ == 2;
  if (mode == 0) luigiGame_ = false;
  luigiTitleFrame(mode);
  titleMode_ = mode;
  // (the menu up: its two choices and the cursor's column untouched; not
  // while the demo's level scrolls in over it)
  auto cursorCol = [&](uint16_t v) { return v == kTitleCursor || v == kTitleBlank || v == 0x0024; };
  const bool was = titleShown_;
  titleShown_ = mode == 0 && vram[L.row1] == L.first && vram[L.row1 + 64] == L.second &&
                cursorCol(vram[L.cursor]) && cursorCol(vram[L.cursor + 64]);
  if (!titleShown_) return;
  // (on 1 PLAYER GAME, as the game's; on CO-OP GAME with co-op on: turned on in the
  // options, 1 PLAYER GAME under Start turned it off again)
  if (!was) titleCursor_ = coop_ ? 2 : 0;
  const uint16_t row3 = (uint16_t)(L.row1 + 128), blank = vram[L.row1 + 32];  // (row 19: empty)
  // (The Lost Levels' small tree, under the row's end: two tiles right, into
  // the clear columns before the tall one)
  if (L.tree) {
    const uint16_t t = (uint16_t)(0x0200 + 6 * 32 + 22);  // (row 22, column 22)
    for (int y = 0; y < 4; y++)
      for (int k = 0; k < 2; k++) {
        uint16_t& from = vram[t + 32 * y + k];
        const int c = from & 0x3ff;
        if (c >= 0xb8 && c <= 0xbf) {
          titleTree_[y][k] = from;
          from = blank;
        }
        if (titleTree_[y][k]) vram[t + 32 * y + k + 2] = titleTree_[y][k];
      }
  }
  vram[row3 + (L.cursor - L.row1)] = titleCursor_ == 2 ? kTitleCursor : kTitleBlank;
  // (the game's own cursor off its rows: placed on CO-OP GAME as the title
  // opened, the game's was still drawn on the first, two shown till a move)
  if (titleCursor_ == 2) vram[L.cursor] = vram[L.cursor + 64] = kTitleBlank;
  for (int i = 0; i < 13 && L.text[i]; i++) vram[row3 + i] = (uint16_t)(0x0800 | L.text[i]);
  // (TOP- 000000, on the row under it, a row further down: the menu's rows
  // two apart, then the score. Moved as it's drawn, tile by tile: the title
  // writes its digits a few frames after the rest.)
  uint16_t* top = &vram[row3 + 32 + L.topFrom];
  for (int i = 0; i < L.topCount; i++)
    if (top[i] != blank) {
      titleTop_[i] = top[i];
      top[i] = blank;
    }
  for (int i = 0; i < L.topCount; i++)
    if (titleTop_[i]) top[32 + i] = titleTop_[i];
}

// The status bar's name (BG3's tile map, row 2 from column 3, MARIO or LUIGI
// over the score) reads MARIO & LUIGI while co-op plays (on the title, while
// its cursor is on CO-OP GAME). The font has no &: one is drawn into a tile
// BG3's map doesn't show, in its style (fill 1, outlined 3). Off again, the
// name it had comes back.
namespace {
constexpr uint16_t kHudName = 0x5843, kHudWorld = 0x5852, kHudChr = 0x5000;
constexpr uint16_t kHudBlank = 0x0028, kHudAttr = 0x2000;
// (the &'s fill, inside the 8x8 tile's outline)
const char* const kAmpFill[8] = {"        ", "  11    ", " 1  1   ", "  11  1 ", " 1  11  ",
                                 " 1   1  ", "  111 1 ", "        "};
}  // namespace

void Smb1Engine::hudFrame() {
  uint16_t* vram = snes_.ppu.vram;
  static const uint16_t kWorld[5] = {0x20, 0x18, 0x1b, 0x15, 0x0d};
  bool hud = true;
  for (int i = 0; i < 5 && hud; i++) hud = (vram[kHudWorld + i] & 0x3ff) == kWorld[i];
  const bool show = hud && (titleShown_ ? titleCursor_ == 2 : coop_ && (ram(0x0770) == 1 || ram(0x0770) == 2));
  uint16_t* name = &vram[kHudName];
  static const uint16_t kMario[5] = {0x16, 0x0a, 0x1b, 0x12, 0x18}, kLuigi[5] = {0x15, 0x1e, 0x12, 0x10, 0x12};
  auto is = [&](const uint16_t* n) {
    for (int i = 0; i < 5; i++)
      if ((name[i] & 0x3ff) != n[i]) return false;
    return true;
  };
  if (!show) {
    // (its own name back; MARIO & LUIGI from a co-op state loaded: the
    // current player's)
    if (hud && !hudDrawn_ && (name[8] & 0x3ff) == 0x15 && (name[12] & 0x3ff) == 0x12 && is(kMario)) {
      const uint16_t* own = ram(0x0753) ? kLuigi : kMario;
      for (int i = 0; i < 5; i++) hudName_[i] = (uint16_t)(kHudAttr | own[i]);
      hudDrawn_ = true;
    }
    if (hudDrawn_ && hud) {
      std::copy(hudName_, hudName_ + 5, name);
      std::fill(name + 5, name + 13, kHudBlank);
    }
    hudDrawn_ = false;
    return;
  }
  if (!hudDrawn_ || is(kMario) || is(kLuigi)) {
    if (is(kMario) || is(kLuigi)) std::copy(name, name + 5, hudName_);  // (the game's, drawn again)
    // (a tile for the &: one BG3's map doesn't show, empty or already it)
    bool used[0x100] = {};
    for (int a = 0x5800; a < 0x5c00; a++)
      if ((vram[a] & 0x3ff) < 0x100 && a != kHudName + 6) used[vram[a] & 0xff] = true;
    if (!hudAmp_ || used[hudAmp_]) {
      hudAmp_ = 0;
      for (int t = 0x80; t < 0x100 && !hudAmp_; t++) {
        bool empty = true;
        for (int i = 0; i < 8 && empty; i++) empty = vram[kHudChr + 8 * t + i] == 0;
        if (!used[t] && empty) hudAmp_ = (uint16_t)t;
      }
    }
    hudDrawn_ = hudAmp_ != 0;
  }
  if (!hudDrawn_) return;
  // (the &: its fill, and the outline round it)
  uint16_t* d = &vram[kHudChr + 8 * hudAmp_];
  for (int y = 0; y < 8; y++) {
    uint8_t p0 = 0, p1 = 0;
    for (int x = 0; x < 8; x++) {
      bool fill = kAmpFill[y][x] == '1', edge = false;
      for (int dy = -1; dy <= 1 && !fill && !edge; dy++)
        for (int dx = -1; dx <= 1 && !edge; dx++) {
          const int yy = y + dy, xx = x + dx;
          edge = yy >= 0 && yy < 8 && xx >= 0 && xx < 8 && kAmpFill[yy][xx] == '1';
        }
      if (fill) p0 |= 0x80 >> x;
      if (edge) p0 |= 0x80 >> x, p1 |= 0x80 >> x;
    }
    d[y] = (uint16_t)(p0 | p1 << 8);
  }
  // M A R I O _ & _ L U I G I
  const uint16_t text[13] = {0x16, 0x0a, 0x1b, 0x12, 0x18, 0, hudAmp_, 0, 0x15, 0x1e, 0x12, 0x10, 0x12};
  for (int i = 0; i < 13; i++) name[i] = text[i] || i == 6 ? (uint16_t)(kHudAttr | text[i]) : kHudBlank;
}

void Smb1Engine::coopLoad(const uint8_t* p, size_t n) {
  boxAfterSwap_ = false;
  restWait_ = false;
  if (!coopOk_ || p2_.empty()) return;
  // (where the game's player is now: from wherever the state came, not a
  // move of the game's (see coopAfter_: Luigi was put back beside Mario))
  lastMe_ = snes_.wram[0x0078] * 256 + snes_.wram[0x0219];
  vicStarted_ = snes_.wram[0x0770] == 2;  // (a state from the victory scene: in it, not starting it again)
  // (States from before [JumpspringForce] joined the variables: one fewer,
  // the rest in order; and from before the last kVarsAdded: those 0.)
  const int spring = varIndex(0x06DB);
  const int count = p && n > 3 ? p[3] : 0;
  const bool older = count == kVarCount - kVarsAdded - 1;
  const int have = older ? count + 1 : count;  // (of the variables in order; those after it 0)
  const size_t need = 4 + count + 12 + 4 + 4 + 512;
  if (!p || n < need || p[0] != 'C' || p[1] != 1 ||
      (count != kVarCount && count != kVarCount - kVarsAdded + kVarsAddedFirst && have != kVarCount - kVarsAdded))
    return;
  const uint8_t flags = p[2];
  p2Valid_ = flags & 1;
  p2Out_ = flags & 2;
  p2Known_ = flags & 4;
  luigiLive_ = flags & 8;
  p2Drawn_ = flags & 16;
  liveCtrl_ = flags & 32;
  p2Wait_ = flags & 64;
  const uint8_t* q = p + 4;
  for (int i = 0, k = 0; i < kVarCount; i++)
    p2_[i] = (older && i == spring) || i >= have ? 0 : q[k++];
  q += count;
  for (int& v : p2Box_) {
    v = (int)(q[0] | q[1] << 8 | q[2] << 16 | (uint32_t)q[3] << 24);
    q += 4;
  }
  std::copy(q, q + 3, p2Buttons_);
  q += 3;
  p2PrevAB_ = *q++;
  p2TileBase_ = (uint16_t)(q[0] | q[1] << 8);
  p2TileWords_ = (uint16_t)(q[2] | q[3] << 8);
  q += 4;
  for (uint16_t& v : p2Tiles_) {
    v = (uint16_t)(q[0] | q[1] << 8);
    q += 2;
  }
  auto left = [&](size_t k) { return (size_t)(q - p) + k <= n; };
  auto u32 = [&]() {
    uint32_t v = (uint32_t)(q[0] | q[1] << 8 | q[2] << 16 | (uint32_t)q[3] << 24);
    q += 4;
    return v;
  };
  for (int k = 0; k < 2; k++) {
    idle_[k] = left(4) ? u32() : 1u << 20;  // (older states: as never touched)
    still_[k] = std::min(idle_[k], 1u << 20);
  }
  stoodOk_[0] = stoodOk_[1] = false;
  deathTune_ = false;
  musicBack_ = 0;
  apuSnap_.clear();
  if (left(11)) {
    deathTune_ = *q++ != 0;
    musicBack_ = (int)std::min<uint32_t>(u32(), 1000);
    musicTrack_ = *q++;
    lastTrack_ = *q++;
    const uint32_t snap = u32();
    if (left(snap)) {
      apuSnap_.assign(q, q + snap);
      q += snap;
    }
  }
  for (int k = 0; k < 10; k++) {
    p2Plat_[k] = 0xff;
    p2Coll_[k] = 0;
  }
  std::fill(std::begin(p2Ham_), std::end(p2Ham_), 0);
  // (Whose entries hold whom, as the state's frame drew them: his colours
  // from the first frame shown, each step of a rewind being one. Else the
  // last frame's before the load were compared, and both were Mario's.)
  saveSprites(0, snes_.wram[0x0B45]);
  saveSprites(1, kP2Oam);
  // (as last seen: the state's, not the frame's before the load)
  lastInterval_ = snes_.wram[0x0787];
  lastP2Inj_ = p2_[varIndex(0x07AE)];
}

// A player's 8 sprites as just drawn (p: 0 the game's player, 1 the other),
// to put back if another object's drawn over them (see coopPack_).
void Smb1Engine::saveSprites(int p, uint8_t offset) {
  const uint8_t* w = snes_.wram;
  sprBase_[p] = offset / 4;
  sprFrame_[p] = snes_.frameCount();
  for (int k = 0; k < 8; k++) {
    const int s = sprBase_[p] + k;
    std::copy(w + 0x0800 + 4 * s, w + 0x0804 + 4 * s, sprSave_[p][k]);
    sprHi_[p][k] = w[0x0C00 + 4 * s];
    sprX_[p][k] = oamX_[s];
    sprXSet_[p][k] = oamXSet_[s];
  }
}

// Objects that meet the player in their own routines, not through
// PlayerEnemyCollision: lifts ($24-$2C: a lift moves its rider, then finds
// who stands on it, [PlatformCollisionFlag] $03A2,X), the jumpspring ($32:
// it compresses under its rider and throws him up) and firebars ($1B-$1F).
// Each one's routine runs again for the other player, from the world as it
// was before its first run, his variables in; the world then goes back to
// how the first run left it, but for him (Luigi fell through lifts, stuck on
// springs, walked through firebars). If only he rides the lift or the
// spring, his run is the world's (a lift that drops under him drops).
namespace {
bool rerunObject(uint8_t id) { return (id >= 0x1B && id <= 0x1F) || (id >= 0x24 && id <= 0x2C) || id == 0x32; }
bool onLift(uint8_t id, uint8_t flag) { return id >= 0x2B ? flag != 0 && flag != 0xff : !(flag & 0x80); }
}  // namespace

void Smb1Engine::coopObjBefore(int x) {
  uint8_t* w = snes_.wram;
  coopObjPhase_ = 0;
  if (x >= 10) return;
  const uint8_t id = w[0x1C + x];
  if (!w[0x10 + x] || id != p2PlatId_[x]) {
    p2Plat_[x] = 0xff;
    p2PlatId_[x] = id;
  }
  if (!coop_ || !p2Valid_ || p2Out_ || p2Wait_ || w[0x000F] != 8 || !w[0x10 + x] || !rerunObject(id)) return;
  std::copy(w, w + 0x2000, objBefore_.begin());
  coopObjSlot_ = x;
  coopObjPhase_ = 1;
}

// (the low 8 KiB of RAM back as saved, but for the stack's page)
void Smb1Engine::restoreLowRam(const std::vector<uint8_t>& from) {
  uint8_t* w = snes_.wram;
  std::copy(from.begin(), from.begin() + 0x100, w);
  std::copy(from.begin() + 0x200, from.end(), w + 0x200);
}

bool Smb1Engine::coopObjAfter() {
  CpuRegs& r = snes_.cpu.r;
  uint8_t* w = snes_.wram;
  const int x = coopObjSlot_;
  if (coopObjPhase_ == 1) {
    coopObjPhase_ = 0;
    if (w[0x000F] != 8) return false;  // (it hurt the game's player: the world's frozen for him)
    std::copy(w, w + 0x2000, objAfter_.begin());
    restoreLowRam(objBefore_);
    swapPlayers(p1Hold_);
    objAB_ = p2_[varIndex(0x000D)];
    w[0x000D] = p2PrevAB_;  // (his A and B as the game has them at this point: a spring's high bounce)
    for (int i = 0; i < 3; i++) w[0x000A + i] = p2Buttons_[i];
    objP2Flag_ = p2Plat_[x];
    objP2Spring_ = p2_[varIndex(0x070E)];
    if (w[0x1C + x] >= 0x24 && w[0x1C + x] <= 0x2C) w[0x03A2 + x] = p2Plat_[x];
    coopRegs_[0] = r.a;
    coopRegs_[1] = r.x;
    coopRegs_[2] = r.y;
    // JSR EnemiesAndLoopsCore again, returning here
    auto push = [&](uint8_t v) {
      w[r.sp & 0x1fff] = v;
      r.sp = r.e ? (uint16_t)(0x100 | ((r.sp - 1) & 0xff)) : (uint16_t)(r.sp - 1);
    };
    const uint16_t ret = (uint16_t)(enemyRet_ - 1);
    push(ret >> 8);
    push(ret & 0xff);
    r.x = (uint16_t)x;
    w[0x009E] = (uint8_t)x;
    r.pc = (uint16_t)(snes_.peek(enemyCall_ + 1) | snes_.peek(enemyCall_ + 2) << 8);
    coopObjPhase_ = 2;
    return true;
  }
  if (coopObjPhase_ != 2) return false;
  coopObjPhase_ = 0;
  const uint8_t id = w[0x1C + x], task = w[0x000F];
  const bool lift = id >= 0x24 && id <= 0x2C;
  if (lift) p2Plat_[x] = w[0x03A2 + x];
  restorePlayer1(p1Hold_);
  p2_[varIndex(0x000D)] = objAB_;
  const bool his = lift ? !onLift(id, objBefore_[0x03A2 + x]) && onLift(id, objP2Flag_)
                        : id == 0x32 && !objBefore_[0x070E] && objP2Spring_;
  if (his) {
    // (his run stands; the game's player as his own run left him)
    for (uint16_t a : kVars) w[a] = objAfter_[a];
    for (uint16_t a : kHitKeep) w[a] = objAfter_[a];
    for (uint16_t a : kKeep) w[a] = objAfter_[a];
    if (lift) w[0x03A2 + x] = objAfter_[0x03A2 + x];
    std::copy(objAfter_.begin() + 0x11E0, objAfter_.begin() + 0x1200, w + 0x11E0);
  } else {
    for (int a = 0x1600; a < 0x1604; a++)  // (his sounds: a firebar's hit)
      if (w[a] && w[a] != objAfter_[a]) objAfter_[a] = w[a];
    restoreLowRam(objAfter_);
  }
  r.a = coopRegs_[0];
  r.x = coopRegs_[1];
  r.y = coopRegs_[2];
  if (task != 8) {  // (hurt: smaller, blinking, as from an enemy; or killed)
    p2_[varIndex(0x0754)] = p2_[varIndex(0x0756)] ? 0 : 1;
    p2_[varIndex(0x070B)] = 0;
    if (task == 0x0B) p2Lose();
  }
  return false;
}

// Game over with the other character the game's player (The Lost Levels):
// back to the one the game was started with, whose game the continue is
// (it went on as a Luigi game from a Mario one).
void Smb1Engine::coopFrame() {
  const bool s2 = snes_.input[1] & kPadStart;  // (the second controller's Start, newly pressed: see coopEndStart_)
  {  // (... and all its buttons newly pressed, as $0FF6 has the first's: see coopMenu_)
    const uint8_t hi = (uint8_t)(snes_.input[1] >> 8);
    menuPress2_ = (uint8_t)(hi & ~menuHeld2_);
    menuHeld2_ = hi;
  }
  start2_ = s2 && !prevStart2_;
  prevStart2_ = s2;
  if (ram(0x0770) != 2) vicStarted_ = false;
  // Super Mario Bros.' ending (8-4's victory: the portrait, the thanks) is
  // [CurrentPlayer]'s, which co-op leaves Mario: Luigi's when he took the axe.
  if (!lostLevels_) {
    const bool ending = coop_ && ram(0x0770) == 2 && ram(0x075F) == 7 && luigiLive_;
    if (ending) {
      snes_.wram[0x0753] = 1;
      endingLuigi_ = true;
    } else if (endingLuigi_) {
      snes_.wram[0x0753] = 0;
      endingLuigi_ = false;
    }
  }
  if (coop_ && lostLevels_ && liveCtrl_ && ram(0x0770) == 3 && !p2_.empty()) swapRoles();
  // (Super Mario Bros.: the game's player in Luigi's own sprites when he's Luigi (see p2Sync))
  if (!lostLevels_ && coop_) snes_.wram[0x0EC2] = luigiLive_ ? 1 : 0;
  // (the player kept aside hit: his injury timer starts)
  if (coop_ && !p2_.empty()) {
    const uint8_t inj = p2_[varIndex(0x07AE)];
    if (p2Valid_ && inj && !lastP2Inj_) p2Hurts_++;
    lastP2Inj_ = inj;
  }
}

// The kept-aside player's timers, when the game's run: [IntervalTimerControl]
// $0787 counted down this frame; the slower ones as it starts over, at $14.
void Smb1Engine::p2Timers() {
  const uint8_t ic = snes_.wram[0x0787];
  if (ic != lastInterval_) {
    for (uint16_t a : kFrameTimers)
      if (p2_[varIndex(a)]) p2_[varIndex(a)]--;
    if (ic == 0x14)
      for (uint16_t a : kIntervalTimers)
        if (p2_[varIndex(a)]) p2_[varIndex(a)]--;
  }
  lastInterval_ = ic;
}

// The screen held for the leader: the other (at x, on controller pad) is at
// its left edge and playing (not left idle a few seconds: then he's pushed
// along, as before).
bool Smb1Engine::holdScroll(int x, int pad) const {
  // (left idle, he's brought to the other once the other stands, see
  // coopBringIdle: till then the screen waits too, not pushing him on)
  // (or where he last stood, still ahead: see coopBringIdle)
  const int k = pad == (liveCtrl_ ? 1 : 0) ? 1 : 0;
  const bool other = coopStands(k == 1) ||
                     (stoodOk_[k] && stoodAt_[k][0] * 256 + stoodAt_[k][1] >= playerLeftLimit() + 24);
  return x <= playerLeftLimit() + 4 && (idle_[pad] < 180 || !other);
}

// Somewhere to bring one left idle to: the game's player (or Luigi, p2) on
// his feet on the screen, or anywhere on it in water ([SwimmingFlag] $0704:
// a swimmer never lands, and the screen waited for him for good). (The idle
// count isn't restarted by being brought along: it was, and each time the
// screen waited 3 s at its right edge for him again.)
bool Smb1Engine::coopStands(bool p2) const {
  if (p2 && p2_.empty()) return false;
  auto v = [&](uint16_t a) { return p2 ? p2_[varIndex(a)] : ram(a); };
  if (v(0x00BB) != 1) return false;
  return v(0x0028) == 0 || (ram(0x0704) != 0 && v(0x0028) <= 2);
}

bool Smb1Engine::coopExec(uint32_t pc) {
  CpuRegs& r = snes_.cpu.r;
  uint8_t* w = snes_.wram;
  if (pc == coopDeathSmall_) {
    // The game's player killed (small, hurt) after the enemies' run, by what
    // runs after it ($03:BAA2's objects: a cannon's Bullet Bill): that run's
    // end (coopEnemyAt_) had passed, so nothing handed over to the other and
    // the level started over, the other playing (just back, blinking). As
    // there: the other plays on, the dead one back beside him, a life; the
    // death skipped (the routine's end: nothing set yet).
    if (!coop_ || !afterEnemies_ || coopMove_ == 1 || coopEnemy_ >= 0 || w[0x000F] != 8 || w[0x0759] != 0 ||
        !p2Alive(false))
      return false;
    swapRoles();
    p2Lose();
    r.pc = (uint16_t)(coopDeathSmall_ - 3);
    return true;
  }
  if (pc == coopCall_) {
    if (coopMove_ == 1) return false;  // (the other's call: on it goes)
    afterEnemies_ = false;
    for (int i = 0; i < kHitKeepCount; i++) preTurn_[i] = w[kHitKeep[i]];  // (a fall: see coopAfter_)
    // (Each character on his own controller, whichever of them is the game's
    // player: the first for the one the game was started with, the second
    // for the one who joined him.)
    pad1_ = w[0x0FF4];
    pad2_ = w[0x0FF5];
    if (lostLevels_) {
      // (The Lost Levels, a 1-player game, doesn't read the second
      // controller: here, as the game reads the first: B or A jumps, Y or X
      // runs.)
      const uint16_t p = snes_.input[1];
      pad2_ = (uint8_t)(((p & (kPadB | kPadA)) ? 0x80 : 0) | ((p & (kPadY | kPadX)) ? 0x40 : 0) |
                        ((p & kPadUp) ? 0x08 : 0) | ((p & kPadDown) ? 0x04 : 0) | ((p & kPadLeft) ? 0x02 : 0) |
                        ((p & kPadRight) ? 0x01 : 0));
      // (... and not where the game takes the controller away: out of play
      // (the victory scene's walk: Luigi walked off into the pit, the ending
      // lost), or where it reads the first as nothing though held)
      const uint16_t q = snes_.input[0];
      const uint8_t pad1 = (uint8_t)(((q & (kPadB | kPadA)) ? 0x80 : 0) | ((q & (kPadY | kPadX)) ? 0x40 : 0) |
                                     ((q & kPadUp) ? 0x08 : 0) | ((q & kPadDown) ? 0x04 : 0) |
                                     ((q & kPadLeft) ? 0x02 : 0) | ((q & kPadRight) ? 0x01 : 0));
      if (w[0x0770] != 1 || (pad1 && !w[0x0FF4])) pad2_ = 0;
    }
    if (coop_ && liveCtrl_) w[0x0FF4] = pad2_;
    // Mario's turn: he moves the screen only if he is the further right
    // ([ScrollLock] set for his turn, unless the game has set it itself).
    // (... nor while the other stands at the screen's left edge, still
    // playing: the edge pushed him on, into pits)
    for (int k = 0; k < 2; k++) {
      idle_[k] = (k ? pad2_ : pad1_) ? 0 : idle_[k] + 1;
      still_[k] = (k ? pad2_ : pad1_) ? 0 : std::min(still_[k] + 1, 1u << 20);  // (kept when brought along)
    }
    coopBringIdle();
    coopIdleSafe();
    // (Who leads is settled once, here, for both turns: compared before and
    // after Mario moved, a step apart each thought the other led, and the
    // screen stood still.)
    p2Leads_ = p2Valid_ && p2X() > ram(0x0078) * 256 + ram(0x0219);
    lockForced_ = coop_ && p2Valid_ && !p2Wait_ && !p2Out_ && w[0x000F] == 8 && w[0x0723] == 0 &&
                  (p2Leads_ || holdScroll(p2X(), liveCtrl_ ? 0 : 1));
    if (lockForced_) w[0x0723] = 1;
    coopEdge_ = lockForced_;
    return false;
  }
  if (pc == coopAfter_) {
    if (coopMove_ == 2) {
      // His turn of a scene over (see below): what he did stays his; the
      // world goes back to how the game's player's turn left it.
      p2PrevAB_ = w[0x000D];
      w[0x000D] = w[0x000A];
      for (int i = 0; i < 3; i++) p2Buttons_[i] = w[0x000A + i];
      coopMove_ = 0;
      restorePlayer1(p1Hold_);
      restoreLowRam(objBefore_);
      return false;
    }
    if (coopMove_ == 1) {  // Luigi's turn over: Mario back
      coopEdge_ = false;
      // (His skid's dust, drawn where the game's player's goes: to entries
      // 8-11 (no use of the game's), the game's player's back. His bubbles.)
      if (w[0x0E40] || w[0x0E41]) {  // (his dust going: [its step, its start])
        std::copy(w + 0x0830, w + 0x0840, w + 0x0820);
        std::copy(dustHold_, dustHold_ + 16, w + 0x0830);
      }
      coopBubble();
      // (The game keeps this frame's A and B as the last frame's at the end
      // of the frame, from Mario's: Luigi's here. A jump goes on while A is
      // held in both; without it his were all hops.)
      p2PrevAB_ = w[0x000D];  // (his last frame's, for a fireball: see coopFire_)
      w[0x000D] = w[0x000A];
      for (int i = 0; i < 3; i++) p2Buttons_[i] = w[0x000A + i];  // (for his drawing: see below)
      coopMove_ = 0;
      const uint8_t task = w[0x000F];
      // (He took the axe: the victory scene is his (Toad thanks him, The
      // Lost Levels' ending is his); the other walks along (see coopVicDraw_).)
      const bool axe = w[0x0770] == 2;  // (his turn runs in play only: [OperMode] 1)
      if ((task >= 1 && task <= 4) || axe) {
        // He took a vine, a pipe or the flag: the game goes on with him (he
        // goes down the pipe, the screen follows); the other comes along
        // after it. (He stays in; the one he swaps with is kept aside.)
        restorePlayer1(p1Hold_);
        swapRoles();
        // (the scene's own set-up stays: the task, the screen's lock, the
        // controller bits the frame reads)
        for (int i = 0; i < kKeepCount; i++)
          if (kKeep[i] != 0x000F && kKeep[i] != 0x0723 && kKeep[i] > 0x000C && kKeep[i] != 0x0E67)
            w[kKeep[i]] = keepHold_[i];
        for (int i = 0; i < kHitKeepCount; i++) preLoop_[i] = w[kHitKeep[i]];
        return false;
      }
      restorePlayer1(p1Hold_);
      for (int i = 0; i < kKeepCount; i++) w[kKeep[i]] = keepHold_[i];  // (the scroll lock too)
      for (int i = 0; i < kHitKeepCount; i++) preLoop_[i] = w[kHitKeep[i]];  // (see coopEnemyAt_)
      return false;
    }
    if (lockForced_) w[0x0723] = 0;
    lockForced_ = false;
    coopEdge_ = false;
    for (int i = 0; i < kHitKeepCount; i++) preLoop_[i] = w[kHitKeep[i]];
    if (!coop_) return false;
    // Mario down a hole while Luigi plays on: back beside Luigi (a life),
    // what the fall set up undone (the death music, the screen's lock). (Not
    // from a cloud area: that fall takes him back to the level.)
    // ([Player_Y_HighPos] signed, as the game reads it: $FF is above the screen, a spring's
    // launch in The Lost Levels, not down a hole)
    if ((int8_t)ram(0x00BB) >= 2 && w[0x000F] == 8 && !w[0x0743] && !w[0x0759] && p2Alive(false)) {
      for (int i = 0; i < kHitKeepCount; i++) w[kHitKeep[i]] = preTurn_[i];
      for (int i = 0; i < kHitKeepCount; i++) preLoop_[i] = w[kHitKeep[i]];
      swapRoles();  // (the other plays on; the fallen one back beside him)
      p2Lose();
    }
    // Luigi plays while Mario is under control; anything that moves on to
    // another scene (a pipe, the flag, a death, the next area) brings him
    // back beside Mario after it.
    const uint8_t task = w[0x000F];
    const bool play = w[0x0770] == 1 && w[0x0772] == 3;
    if (w[0x0770] == 2) return false;  // (he took the axe: the other stays for the victory scene, see coopVicDraw_)
    // (The game moved its player a long way at once (a castle's wrong way
    // round, 1024 px back): the other comes along, beside him.)
    const int me0 = ram(0x0078) * 256 + ram(0x0219);
    if (play && task == 8 && p2Valid_ && std::abs(me0 - lastMe_) >= 512) p2Valid_ = false;
    lastMe_ = me0;
    // The scenes the game walks its player through, he goes through beside
    // him: the pipe intro ([PlayerEntranceCtrl] 6 or 7, not coming out of a
    // pipe: [AltEntranceControl] 2), the flag and the walk into the castle.
    // The game's routine for it runs with his variables in; all else it
    // changes (the timers, the next area, the music, the screen) goes back
    // after it: the game's player's scene leads.
    const bool intro = task == 7 && (w[0x0710] == 6 || w[0x0710] == 7) && w[0x0752] != 2;
    if (play && (intro || ((task == 4 || task == 5) && p2Valid_)) && !p2Out_ && !p2Wait_) {
      if (!p2Valid_) p2Sync();
      if ((int8_t)p2_[varIndex(0x00BB)] >= 2) {  // (fell: out until the next area, no life lost)
        p2Valid_ = false;
        p2Out_ = true;
        return false;
      }
      // (Into the castle: where the game's player stopped, out of sight, its
      // door. In he goes too.)
      const int me = ram(0x0078) * 256 + ram(0x0219);
      if (task == 5 && me == doorX_ && ram(0x00BB) == 1 && ram(0x0237) >= 0xAE) {
        if (doorWait_ < 255) doorWait_++;
      } else {
        doorWait_ = 0;
      }
      doorX_ = me;
      if (doorWait_ > 8 && p2X() >= me - 4) {
        p2Valid_ = false;
        p2Out_ = true;
        return false;
      }
      p2Timers();
      std::copy(w, w + 0x2000, objBefore_.begin());
      swapPlayers(p1Hold_);
      w[0x0FF4] = 0;
      w[0x0723] = 1;  // (the screen is the game's player's)
      w[0x0480] |= 1;  // ([Player_CollisionBits]: a wall once bumped (the pole's foot) held him there for good)
      coopMove_ = 2;
      // (at the flag: down the pole if he's on it, else walking on to the
      // castle: the game's walk itself (AutoControlPlayer), its end-of-level
      // routine stops walking once the game's player is in)
      const bool onPole = w[0x0028] == 3 && w[0x0237] < 0x9E;
      const bool behind = w[0x0078] * 256 + w[0x0219] < me - 8;  // (ahead of him: he waits, by the door)
      // (Luigi, kept aside, on the pole as the scene starts: he took it
      // himself. Not once the scene's walk has hopped him onto it.)
      const uint32_t fc = snes_.frameCount();
      if (onPole && !luigiLive_ && sceneFrame_ + 1 != fc) luigiPoles_++;
      sceneFrame_ = fc;
      if (intro || onPole || !coopWalk_) {
        w[0x000F] = intro ? 7 : onPole ? 4 : 5;
        r.pc = (uint16_t)coopCall_;
        return true;
      }
      w[0x000F] = 5;
      // (blocked on the ground, by the flagpole's foot or a step: a hop)
      const int px = w[0x0078] * 256 + w[0x0219];
      sceneStuck_ = (behind && px == sceneLastX_ && w[0x0028] == 0) ? sceneStuck_ + 1 : 0;  // (told to walk, and not moving)
      sceneLastX_ = px;
      if (sceneStuck_ >= 4 && !sceneHop_ && behind) sceneHop_ = 12;
      const uint8_t walk = sceneHop_ ? 0x81 : behind ? 0x01 : 0x00;
      if (sceneHop_) sceneHop_--;
      auto push = [&](uint8_t v) {
        w[r.sp & 0x1fff] = v;
        r.sp = r.e ? (uint16_t)(0x100 | ((r.sp - 1) & 0xff)) : (uint16_t)(r.sp - 1);
      };
      const uint16_t ret = (uint16_t)(coopAfter_ - 1);  // (JSR AutoControlPlayer with A = right, back to after the call)
      push(ret >> 8);
      push(ret & 0xff);
      r.a = (r.a & 0xff00) | walk;
      r.pc = (uint16_t)coopWalk_;
      return true;
    }
    if (!play || task <= 7 || task == 0x0B) {
      p2Valid_ = false;
      if (task <= 7) p2Out_ = p2Wait_ = false;  // (a new area or try: back in)
      return false;
    }
    if (task != 8 || p2Out_) return false;  // (changing size, blinking: he waits; out of lives: out)
    if (p2Wait_) {  // (lost a life: back once the other is on his feet)
      if (!p2CanReturn()) return false;
      p2Return();
    }
    if (!p2Valid_) p2Sync();
    if ((int8_t)p2_[varIndex(0x00BB)] >= 2) {  // (down a hole: a death)
      // (off a cloud area's end, its way out: no death, he waits for the
      // level the game goes back to)
      if (w[0x0743]) {
        p2Valid_ = false;
        p2Out_ = true;
      } else {
        p2Lose();
      }
      return false;
    }
    p2Timers();
    for (int i = 0; i < kKeepCount; i++) keepHold_[i] = w[kKeep[i]];
    const bool lead = p2Leads_;
    const bool hold = holdScroll(ram(0x0078) * 256 + ram(0x0219), liveCtrl_ ? 1 : 0);
    swapPlayers(p1Hold_);
    w[0x0FF4] = liveCtrl_ ? pad1_ : pad2_;  // (his controller, as the game reads them)
    if (!lead || hold) w[0x0723] = 1;
    coopEdge_ = !lead || hold;
    coopMove_ = 1;
    std::copy(w + 0x0830, w + 0x0840, dustHold_);
    r.pc = (uint16_t)coopCall_;
    return true;
  }
  if (pc == coopEnemyAt_) {
    // After the enemy loop: each enemy (and power-up) meets Luigi too, the
    // game's PlayerEnemyCollision called for it with his variables in.
    if (coopEnemy_ >= 0) {
      // (back from one: what it did to Luigi. Hurt or powered up, the game
      // freezes the world for the player's size change: here it doesn't, he
      // just changes size (a hit leaves him blinking); a fall to his death
      // brings him back beside Mario.)
      const uint8_t task = w[0x000F];
      if (task != 8) {
        if (task == 0x0B) p2Dead_ = true;
        w[0x0754] = w[0x0756] ? 0 : 1;  // [PlayerSize] from [PlayerStatus]
        w[0x070B] = 0;                  // [PlayerChangeSizeFlag]
        for (int i = 0; i < kHitKeepCount; i++) w[kHitKeep[i]] = hitHold_[i];
      }
      coopEnemy_++;
    } else {
      afterEnemies_ = true;  // (a death from here on: see coopDeathSmall_)
      // Mario killed by an enemy while Luigi plays on: back beside Luigi (a
      // life), the death undone (as it was before the enemies' turn).
      if (coop_ && w[0x000F] == 0x0B && w[0x0759] == 0 && p2Alive(false)) {
        for (int i = 0; i < kHitKeepCount; i++) w[kHitKeep[i]] = preLoop_[i];
        swapRoles();  // (the other plays on; the dead one back beside him)
        p2Lose();
      }
      if (!coop_ || !p2Valid_ || p2Out_ || w[0x000F] != 8 || w[0x0747]) return false;
      for (int i = 0; i < kHitKeepCount; i++) hitHold_[i] = w[kHitKeep[i]];
      std::copy(w + 0x11E0, w + 0x1200, palHold_);  // (Mario's palette: a hit reloads it from Luigi's state)
      coopRegs_[0] = r.a;
      coopRegs_[1] = r.x;
      coopRegs_[2] = r.y;
      swapPlayers(p1Hold_);
      // (Each enemy's "touching the player" bit, his own: shared, Mario's
      // not touching a shell Luigi had just kicked made it a new touch, and
      // the moving shell hurt him.)
      if (collBits_)
        for (int k = 0; k < 10; k++) {
          uint8_t& b = w[collBits_ + k];
          if (!w[0x10 + k]) p2Coll_[k] = 0;
          p1Coll_[k] = b & 1;
          b = (uint8_t)((b & ~1) | p2Coll_[k]);
        }
      coopEnemy_ = 0;
    }
    // (enemies proper, IDs below $15, Bowser's flame ($15), Bowser ($2D), the
    // power-ups ($2E) and cannons' Bullet Bills ($33): not lifts, springs,
    // firebars or the flag, whose own routines meet the player (see
    // coopObjAfter_))
    auto meets = [](uint8_t id) { return id <= 0x15 || id == 0x2D || id == 0x2E || id == 0x33; };
    while (coopEnemy_ < 10 && !(w[0x10 + coopEnemy_] && meets(w[0x1C + coopEnemy_])))
      coopEnemy_++;
    if (coopEnemy_ >= 10 || p2Dead_) {
      if (collBits_)
        for (int k = 0; k < 10; k++) {
          uint8_t& b = w[collBits_ + k];
          p2Coll_[k] = b & 1;
          b = (uint8_t)((b & ~1) | p1Coll_[k]);
        }
      restorePlayer1(p1Hold_);
      std::copy(palHold_, palHold_ + 32, w + 0x11E0);
      r.a = coopRegs_[0];
      r.x = coopRegs_[1];
      r.y = coopRegs_[2];
      coopEnemy_ = -1;
      if (p2Dead_) {
        p2Lose();
        p2Dead_ = false;
      }
      return false;
    }
    // JSR PlayerEnemyCollision with X (and ObjectOffset) the enemy, returning here
    auto push = [&](uint8_t v) {
      w[r.sp & 0x1fff] = v;
      r.sp = r.e ? (uint16_t)(0x100 | ((r.sp - 1) & 0xff)) : (uint16_t)(r.sp - 1);
    };
    const uint16_t ret = (uint16_t)(coopEnemyAt_ - 1);
    push(ret >> 8);
    push(ret & 0xff);
    r.x = (uint16_t)coopEnemy_;
    w[0x009E] = (uint8_t)coopEnemy_;
    r.pc = (uint16_t)coopPEC_;
    return true;
  }
  if (pc == coopLoop_) {
    if (!coop_ || !p2Valid_ || p2Out_ || p2Wait_) return false;
    const uint8_t want = snes_.peek((coopLoop_ & 0xff0000) | (uint16_t)(snes_.peek16(coopLoop_ + 4) + (r.y & 0xff)));
    const bool game = w[0x0237] == want && w[0x0028] == 0;
    const bool other = p2_[varIndex(0x0237)] == want && p2_[varIndex(0x0028)] == 0;
    if (game || !other) return false;
    r.pc = (uint16_t)(coopLoop_ + 14);  // (the test passed)
    return true;
  }
  if (pc == coopWhirlRet_) {
    // After ProcessWhirlpools (in a water area, the whirlpools pull the game's
    // player only, and set [Whirlpool_Flag] for his swim stroke): its loop
    // again with the other player in, his own flag cleared first as the game
    // clears it (the water palette's step once). Luigi swam through them.
    if (coopWhirlPhase_ == 1) {
      restorePlayer1(p1Hold_);
      r.a = coopRegs_[0];
      r.x = coopRegs_[1];
      r.y = coopRegs_[2];
      coopWhirlPhase_ = 0;
      return false;
    }
    if (!coop_ || !p2Valid_ || p2Out_ || p2Wait_ || w[0x000F] != 8 || w[0x005C] != 0) return false;
    p2_[varIndex(0x027D)] = 0;
    if (w[0x0747]) return false;  // ([TimerControl]: the game's leaves before the loop)
    coopRegs_[0] = r.a;
    coopRegs_[1] = r.x;
    coopRegs_[2] = r.y;
    swapPlayers(p1Hold_);
    auto push = [&](uint8_t v) {
      w[r.sp & 0x1fff] = v;
      r.sp = r.e ? (uint16_t)(0x100 | ((r.sp - 1) & 0xff)) : (uint16_t)(r.sp - 1);
    };
    const uint16_t ret = (uint16_t)(coopWhirlRet_ - 1);
    push(ret >> 8);
    push(ret & 0xff);
    coopWhirlPhase_ = 1;
    r.pc = (uint16_t)coopWhirl_;
    return true;
  }
  if (pc == coopFire_) {
    // After the game's fireball step (Mario's throw; both fireballs moved,
    // but only while Mario has fire power): Luigi's throw, as the game would
    // with his fire power and B newly pressed, his new fireball started from
    // him (FireballObjCore with his variables in); and with Mario not fiery,
    // the fireballs in flight moved here (they stood in the air).
    // (One call at a time, each returning here: see coopFireQ_.)
    if (coopFirePhase_ == 1) {
      if (coopFireSwap_) restorePlayer1(p1Hold_);
      coopFireSwap_ = false;
    } else {
      coopFireN_ = coopFireI_ = 0;
      if (!coop_ || !p2Valid_ || p2Out_ || w[0x000F] != 8) return false;
      int thrown = -1;
      const int x = w[0x06CE] & 1;  // [FireballCounter]
      if (p2_[varIndex(0x0756)] == 2 && (p2Buttons_[0] & 0x40) && !(p2PrevAB_ & 0x40) && !w[0x33 + x] &&
          p2_[varIndex(0x00BB)] == 1 && !p2_[varIndex(0x0714)] && p2_[varIndex(0x0028)] != 3) {
        w[0x1603] = 0x06;    // (the fireball's sound)
        w[0x33 + x] = 0x02;  // [Fireball_State]: to start from the player
        p2_[varIndex(0x0711)] = p2_[varIndex(0x070C)];                 // [FireballThrowingTimer]
        p2_[varIndex(0x0789)] = (uint8_t)(p2_[varIndex(0x070C)] - 1);  // [PlayerAnimTimer]
        w[0x06CE]++;
        thrown = x;
      }
      if (w[0x0756] < 2)
        for (int k = 0; k < 2; k++)
          if (k != thrown && w[0x33 + k]) coopFireQ_[coopFireN_++] = k;
      if (thrown >= 0) coopFireQ_[coopFireN_++] = 0x80 | thrown;
      if (!coopFireN_) return false;
      coopRegs_[0] = r.a;
      coopRegs_[1] = r.x;
      coopRegs_[2] = r.y;
    }
    if (coopFireI_ >= coopFireN_) {
      r.a = coopRegs_[0];
      r.x = coopRegs_[1];
      r.y = coopRegs_[2];
      coopFirePhase_ = 0;
      coopFireN_ = coopFireI_ = 0;
      return false;
    }
    // JSR FireballObjCore with X the fireball, returning here
    const int q = coopFireQ_[coopFireI_++];
    coopFireSwap_ = q & 0x80;
    if (coopFireSwap_) swapPlayers(p1Hold_);
    auto push = [&](uint8_t v) {
      w[r.sp & 0x1fff] = v;
      r.sp = r.e ? (uint16_t)(0x100 | ((r.sp - 1) & 0xff)) : (uint16_t)(r.sp - 1);
    };
    const uint16_t ret = (uint16_t)(coopFire_ - 1);
    push(ret >> 8);
    push(ret & 0xff);
    r.x = (uint16_t)(q & 1);
    coopFirePhase_ = 1;
    r.pc = (uint16_t)coopFbCore_;
    return true;
  }
  if (pc == coopHammer_) {
    // PlayerHammerCollision (a hammer's own meeting with the player): run for
    // the game's player, back here, run again with the other player in (his
    // own [Misc_Collision_Flag]), back here, then on to its RTS.
    auto push = [&](uint8_t v) {
      w[r.sp & 0x1fff] = v;
      r.sp = r.e ? (uint16_t)(0x100 | ((r.sp - 1) & 0xff)) : (uint16_t)(r.sp - 1);
    };
    const uint16_t ret = (uint16_t)(coopHammer_ - 1);
    if (coopHamPhase_ == 0) {
      if (!coop_ || !p2Valid_ || p2Out_ || p2Wait_ || w[0x000F] != 8 || (r.x & 0xff) >= 16) return false;
      coopHamSlot_ = r.x & 0xff;
      coopHamPhase_ = 1;
      for (int i = 0; i < kHitKeepCount; i++) hamPre_[i] = w[kHitKeep[i]];  // (a death's set-up, undone below)
      push(ret >> 8);
      push(ret & 0xff);
      return false;
    }
    const int x = coopHamSlot_;
    if (coopHamPhase_ == 1) {
      coopRegs_[0] = r.a;
      coopRegs_[1] = r.x;
      coopRegs_[2] = r.y;
      if (w[0x000F] != 8) {  // (the game's player hurt: the world's frozen)
        // (killed while the other plays on: back beside him, a life, as
        // from an enemy; the hammers' run comes after the enemies')
        if (w[0x000F] == 0x0B && !w[0x0759] && p2Alive(false)) {
          for (int i = 0; i < kHitKeepCount; i++) w[kHitKeep[i]] = hamPre_[i];
          swapRoles();
          p2Lose();
        }
        coopHamPhase_ = 0;
        r.pc = (uint16_t)coopHammerRts_;
        return true;
      }
      for (int i = 0; i < kHitKeepCount; i++) hitHold_[i] = w[kHitKeep[i]];
      std::copy(w + 0x11E0, w + 0x1200, palHold_);
      swapPlayers(p1Hold_);
      hamP1_ = w[hamFlag_ + x];
      w[hamFlag_ + x] = p2Ham_[x];
      r.x = (uint16_t)x;
      w[0x009E] = (uint8_t)x;
      coopHamPhase_ = 2;
      push(ret >> 8);
      push(ret & 0xff);
      return false;
    }
    coopHamPhase_ = 0;
    p2Ham_[x] = w[hamFlag_ + x];
    w[hamFlag_ + x] = hamP1_;
    bool dead = false;
    if (w[0x000F] != 8) {
      dead = w[0x000F] == 0x0B;
      w[0x0754] = w[0x0756] ? 0 : 1;
      w[0x070B] = 0;
      for (int i = 0; i < kHitKeepCount; i++) w[kHitKeep[i]] = hitHold_[i];
    }
    restorePlayer1(p1Hold_);
    std::copy(palHold_, palHold_ + 32, w + 0x11E0);
    if (dead) p2Lose();
    r.a = coopRegs_[0];
    r.x = coopRegs_[1];
    r.y = coopRegs_[2];
    r.pc = (uint16_t)coopHammerRts_;
    return true;
  }
  if (pc == coopStar_) {
    // The other's star over (the game's player has none): the area's music
    // back, as the game does for its player's (JSL, returning here). The
    // same after the other's death: the death tune (as KillPlayer queues it,
    // $1602 = 9), and the area's music once it's over, the game's player
    // still playing on (without a star: its end brings the music back).
    if (starMusic_) {
      starMusic_ = false;
      return false;
    }
    if (deathTune_) {
      deathTune_ = false;
      if (coop_ && w[0x000F] == 8) {
        // (the sound chip from before the death: the game's own death may have
        // sent the tune already, at the last frame's end)
        apuSnap_ = lastApu_;
        musicTrack_ = lastTrack_;
        w[0x1602] = 0x09;
        musicBack_ = 160;  // (the tune's length)
      }
      return false;
    }
    if (coop_ && musicBack_ == 0 && w[0x1602] == 0 && w[0x1606] != 0x09) {
      lastApu_ = snes_.apu.saveState();
      lastTrack_ = w[0x1606];
    }
    if (musicBack_ > 0 && w[0x000F] != 8) musicBack_ = 0;
    const bool back = musicBack_ > 0 && --musicBack_ == 0;
    // (The tune over with the game's player starred: his star's music again;
    // its end brings the area's back. It was silent until then.)
    if (back && w[0x07AF] > 4) {
      w[0x1602] = 0x0D;
      apuSnap_.clear();
      return false;
    }
    // (The game's player's star ending with the other's going on: not the
    // area's music yet (the game's JSL passed); the other's end brings it.)
    if (!back && coop_ && p2Valid_ && !p2Out_ && w[0x07AF] == 4 && !w[0x0787] && p2_[varIndex(0x07AF)] > 4) {
      if (!musicBack_) apuSnap_.clear();
      r.pc = (uint16_t)(coopStar_ + 18);
      return true;
    }
    // The tune over, nothing else since (the music last sent, $1606, still
    // it): the sound chip back as it was, the area's music going on where it
    // stopped, not from its start (its clock kept; its ports as it had seen
    // them: the music port still holding the tune would have played it again;
    // the game never waits on the chip, only clears a stale echo).
    // Else the area's music again, from the game's routine.
    if (back && !apuSnap_.empty() && w[0x1606] == 0x09 && musicTrack_ != 0x09) {
      const uint64_t now = snes_.apu.cycles();
      if (snes_.apu.loadState(apuSnap_.data(), apuSnap_.size())) {
        snes_.apu.setCycles(now);
        w[0x1606] = musicTrack_;
        apuSnap_.clear();
        return false;
      }
    }
    if (!musicBack_) apuSnap_.clear();
    if (!back && (!coop_ || !p2Valid_ || w[0x07AF] || p2_[varIndex(0x07AF)] != 4 || w[0x0787])) return false;
    auto push = [&](uint8_t v) {
      w[r.sp & 0x1fff] = v;
      r.sp = r.e ? (uint16_t)(0x100 | ((r.sp - 1) & 0xff)) : (uint16_t)(r.sp - 1);
    };
    const uint16_t ret = (uint16_t)(coopStar_ - 1);
    push(r.pb);
    push(ret >> 8);
    push(ret & 0xff);
    r.pb = (uint8_t)(coopStarMusic_ >> 16);
    r.pc = (uint16_t)coopStarMusic_;
    starMusic_ = true;
    return true;
  }
  if (pc == coopVicDraw_) {
    // The victory scene (after the axe), at the game's player's drawing:
    // the other walks on beside him (the game's own walk, AutoControlPlayer,
    // all else it changes undone, as in the castle walk), stops beside him
    // at Toad, and is drawn too (GameEngine's drawing, back here). Into the
    // lava with the bridge: out of the scene, no life lost.
    auto push = [&](uint8_t v) {
      w[r.sp & 0x1fff] = v;
      r.sp = r.e ? (uint16_t)(0x100 | ((r.sp - 1) & 0xff)) : (uint16_t)(r.sp - 1);
    };
    const uint16_t ret = (uint16_t)(coopVicDraw_ - 1);
    if (vicPhase_ == 0) {
      if (!coop_ || !p2Valid_ || p2Out_ || p2Wait_ || w[0x0770] != 2 || !coopWalk_) return false;
      coopRegs_[0] = r.a;
      coopRegs_[1] = r.x;
      coopRegs_[2] = r.y;
      const int me = ram(0x0078) * 256 + ram(0x0219);
      // (Fallen, or off the screen (8-4's end, the princess's room): back
      // beside him. And once he has stopped (at Toad, at the princess), the
      // other not by him is put there.)
      vicStill_ = me == vicLastMe_ ? vicStill_ + 1 : 0;
      vicLastMe_ = me;
      const int py = p2_[varIndex(0x0237)], hi = p2_[varIndex(0x00BB)];
      const bool lost = hi != 1 || py < 0x10;
      const bool apart = std::abs(p2X() - me) > 40 || std::abs(py - ram(0x0237)) > 8;
      if (lost || (vicStill_ == 30 && apart)) {
        p2Sync();
        vicStarted_ = true;
      }
      if (!vicStarted_) {
        // (Behind him on the bridge as it goes: only its picture goes, he
        // walked on over the lava. He comes along, as after a pipe: where
        // the axe was taken.)
        vicStarted_ = true;
        if (p2X() < me) {
          p2Sync();
          p2_[varIndex(0x0219)] = ram(0x0219);
          p2_[varIndex(0x0078)] = ram(0x0078);
          p2Box_[0] = boxL_[0];
          p2Box_[1] = boxR_[0];
        }
      }
      p2Timers();
      std::copy(w, w + 0x2000, objBefore_.begin());
      swapPlayers(p1Hold_);
      w[0x0723] = 1;
      vicPhase_ = 1;
      push(ret >> 8);
      push(ret & 0xff);
      // (on to beside him, braking his skid short of him: off a ledge at a
      // run he went over into the lava by the princess)
      const int px = p2X();
      const uint8_t speed = w[0x005D];
      // (Walked on only with ground ahead, a step down at most three tiles:
      // in 8-4's last room he walked off the step into the lava; he waits,
      // brought beside the game's player once that one stands, see above.)
      const int fy = w[0x0237];
      bool ground = false;
      for (int k = 1; k <= 3 && !ground; k++) ground = metatile(px + 24, fy + 20 + 16 * k) > 0;
      const bool walk = px < me - 40 && (ground || metatile(px + 24, fy + 36) < 0);
      r.a = (r.a & 0xff00) | (walk ? 0x01 : (speed && speed < 0x80 && px > me - 32) ? 0x02 : 0x00);
      r.pc = (uint16_t)coopWalk_;
      return true;
    }
    if (vicPhase_ == 1) {
      for (int i = 0; i < 3; i++) p2Buttons_[i] = w[0x000A + i];
      p2PrevAB_ = w[0x000D];
      restorePlayer1(p1Hold_);
      restoreLowRam(objBefore_);
      swapPlayers(p1Hold_);
      for (int i = 0; i < 3; i++) {
        buttonHold_[i] = w[0x000A + i];
        w[0x000A + i] = p2Buttons_[i];
      }
      coopP2OamClear();
      coopDrawPhase_ = 1;
      vicPhase_ = 2;
      push(ret >> 8);
      push(ret & 0xff);
      r.pc = (uint16_t)coopDraw_;
      return true;
    }
    vicPhase_ = 0;
    r.a = coopRegs_[0];
    r.x = coopRegs_[1];
    r.y = coopRegs_[2];
    return false;
  }
  if (pc == coopMenu_[0] || pc == coopMenu_[1] || pc == coopMenu_[2]) {
    // (The menus after an ending and after a game over (Continue, Save &
    // Continue...) read the first controller only (Super Mario Bros.' game
    // over's takes the second's too; The Lost Levels never reads it): the
    // other's newly pressed buttons with it, in co-op.)
    if (!coop_) return false;
    const bool indexed = snes_.peek(pc) == 0xBD;
    const uint8_t v = (uint8_t)(w[0x0FF6 + (indexed ? (r.x & 0xff) : 0)] | menuPress2_);
    r.a = (r.a & 0xff00) | v;
    r.n = v & 0x80;
    r.z = v == 0;
    r.pc += 3;
    return true;
  }
  if (pc == coopEndStart_) {
    // (The ending waits for its player's Start (Super Mario Bros.: by
    // [CurrentPlayer]; The Lost Levels: the first controller's): either
    // player's in co-op.)
    if (!coop_) return false;
    uint8_t v = w[0x0FF6] | (snes_.peek(coopEndStart_) == 0xBD ? w[0x0FF6 + (r.x & 0xff)] : 0) | (start2_ ? 0x10 : 0);
    if (snes_.peek(coopEndStart_) == 0xBD) v |= w[0x0FF7];
    r.a = (r.a & 0xff00) | v;
    r.n = v & 0x80;
    r.z = v == 0;
    r.pc += 3;
    return true;
  }
  if (pc == coopThanks_) {
    // (Toad's thanks, by [CurrentPlayer]: Super Mario Bros.' co-op players
    // are both its Mario; Luigi's when he took the axe)
    if (!coop_ || !luigiLive_ || lostLevels_) return false;
    r.a = (r.a & 0xff00) | 1;
    r.n = false;
    r.z = false;
    r.pc += 3;
    return true;
  }
  if (pc == coopPack_) {
    // The frame's sprites all drawn: the game hands each object a run of 6
    // OAM entries (the bumped blocks' runs, $C0 and $D8, lie across the
    // players'), drawn after the players: whatever went over a player's
    // sprites moves to a free entry, and his are put back. (Luigi lost
    // sprites to a bumped brick, which took his colours.)
    if (!coop_) return false;
    const uint32_t f = snes_.frameCount();
    // (The level card: WORLD 1-1, the player's head and his lives ([OperMode]
    // 1, [OperMode_Task] below 3, the head's tiles streamed: Mario's from
    // $0A:9A00; Luigi's, The Lost Levels' as its player, from $0A:DA00).
    // Both heads are shown: see coopLine.)
    if (w[0x0770] == 1 && w[0x0772] < 3 && w[0x028F] == 0x00 && (w[0x0290] == 0x9A || w[0x0290] == 0xDA) &&
        w[0x0291] == 0x0A) {
      cardFrame_ = f;
      cardLuigi_ = w[0x0290] == 0xDA;
    }
    auto reserved = [&](int s) {
      for (int p = 0; p < 2; p++)
        if (sprFrame_[p] == f && s >= sprBase_[p] && s < sprBase_[p] + 8) return true;
      return false;
    };
    int freeAt = 127;
    for (int p = 0; p < 2; p++) {
      if (sprFrame_[p] != f) continue;
      for (int k = 0; k < 8; k++) {
        const int s = sprBase_[p] + k;
        uint8_t* e = w + 0x0800 + 4 * s;
        uint8_t& hi = w[0x0C00 + 4 * s];
        if (std::equal(e, e + 4, sprSave_[p][k]) && hi == sprHi_[p][k]) continue;
        if (e[1] < 0xE0) {  // (someone else's sprite: to a free entry)
          while (freeAt >= 0 && (reserved(freeAt) || w[0x0801 + 4 * freeAt] < 0xE0)) freeAt--;
          if (freeAt >= 0) {
            std::copy(e, e + 4, w + 0x0800 + 4 * freeAt);
            w[0x0C00 + 4 * freeAt] = hi;
            oamX_[freeAt] = oamX_[s];
            oamXSet_[freeAt] = oamXSet_[s];
            freeAt--;
          }
        }
        std::copy(sprSave_[p][k], sprSave_[p][k] + 4, e);
        hi = sprHi_[p][k];
        oamX_[s] = sprX_[p][k];
        oamXSet_[s] = sprXSet_[p][k];
      }
    }
    return false;
  }
  if (pc == coopLeftHi_) {
    // (Luigi across the left edge: his own sprites' bit 8, not Mario's; that
    // cut Mario in half, his left column 256 px away.)
    if (coopDrawPhase_ != 1) return false;
    const int o = 0x0C00 + w[0x0B45];
    for (int k = 0; k < 4; k++) w[o + 8 * k] = 1;
    r.pc = (uint16_t)(coopLeftHi_ + 14);  // (its RTS)
    return true;
  }
  if (pc == coopDrawEnd_) {
    if (coopDrawPhase_ == 1) {
      restorePlayer1(p1Hold_);
      for (int i = 0; i < 3; i++) w[0x000A + i] = buttonHold_[i];
      coopDrawPhase_ = 0;
      p2Drawn_ = true;
      saveSprites(1, kP2Oam);
      // Luigi's animation frame: the tiles PlayerGfxHandler set up to stream
      // (the console streams Mario's), from ROM.
      if (p2_[varIndex(0x028E)]) {
        uint32_t src = p2_[varIndex(0x0291)] << 16 | p2_[varIndex(0x0290)] << 8 | p2_[varIndex(0x028F)];
        int bytes = std::min(512, p2_[varIndex(0x0293)] | p2_[varIndex(0x0294)] << 8);
        p2TileBase_ = (uint16_t)(p2_[varIndex(0x0295)] | p2_[varIndex(0x0296)] << 8);
        p2TileWords_ = (uint16_t)(bytes / 2);
        for (int i = 0; i < bytes / 2; i++)
          p2Tiles_[i] = (uint16_t)(snes_.peek(src + 2 * i) | snes_.peek(src + 2 * i + 1) << 8);
      }
      if (vicPhase_ == 2) {  // (drawn for the victory scene: back to it, as from a JSR)
        const uint16_t sp1 = r.e ? (uint16_t)(0x100 | ((r.sp + 1) & 0xff)) : (uint16_t)(r.sp + 1);
        const uint16_t sp2 = r.e ? (uint16_t)(0x100 | ((r.sp + 2) & 0xff)) : (uint16_t)(r.sp + 2);
        r.pc = (uint16_t)((w[sp1 & 0x1fff] | w[sp2 & 0x1fff] << 8) + 1);
        r.sp = sp2;
        return true;
      }
      return false;
    }
    p2Drawn_ = false;
    if (!coop_) return false;
    saveSprites(0, w[0x0B45]);  // (the game's player's, just drawn: see coopPack_)
    if (!p2Valid_) return false;
    swapPlayers(p1Hold_);
    // (His pose is chosen from his controller too: standing only with no
    // left or right held. Mario's walking had him run on the spot.)
    for (int i = 0; i < 3; i++) {
      buttonHold_[i] = w[0x000A + i];
      w[0x000A + i] = p2Buttons_[i];
    }
    coopP2OamClear();
    coopDrawPhase_ = 1;
    r.pc = (uint16_t)coopDraw_;
    return true;
  }
  return false;
}

// Before his drawing, in the victory scene: his sprite entries (44-51) are
// where All-Stars draws the princess ($08B0/$08B4, $04:DCED), drawn first;
// his overwrote her (she was missing from her cage) and took her size bits
// (a 16x16 tile's garbage over his head). What's there goes to entries the
// scene leaves free (1-15), and his size bits are cleared.
void Smb1Engine::coopP2OamClear() {
  uint8_t* w = snes_.wram;
  if (w[0x0770] != 2) return;
  const int base = w[0x0B45];
  int f = 0x04;
  for (int k = 0; k < 8; k++) {
    const int off = (base + 4 * k) & 0x1ff;
    if (w[0x0801 + off] < 0xF0) {
      while (f < 0x40 && w[0x0801 + f] < 0xF0) f += 4;
      if (f < 0x40) {
        for (int i = 0; i < 4; i++) w[0x0800 + f + i] = w[0x0800 + off + i];
        w[0x0C00 + f] = w[0x0C00 + off];
        f += 4;
      }
      w[0x0801 + off] = 0xF8;
    }
    w[0x0C00 + off] = 0;
  }
}

// Each character in his own colours: the game's own (the palettes it gives
// Mario and Luigi in a 2-player game, normal or fire; flashing with a star).
// The player kept aside (OAM entries 44-51) always takes them here; the
// game's player (52-59) when he is Luigi (the game colours him as Mario).
void Smb1Engine::coopLine(Ppu& ppu, int row) {
  auto colours = [&](bool luigi, uint8_t status, uint8_t star, uint16_t* out) {
    uint32_t pal = luigi ? (status == 2 ? kLuigiFire : kLuigiPal) : (status == 2 ? kMarioFire : kMarioPal);
    if (star) {  // (a star: the game's star palettes in turn)
      int k = (snes_.frameCount() >> 1) & 3;
      if (k < 3) pal = kStarPals + 32 * k;
    }
    for (int i = 0; i < 16; i++) out[i] = (uint16_t)(snes_.peek(pal + 2 * i) | snes_.peek(pal + 2 * i + 1) << 8);
    // (each in the colours chosen for him: see setCharacterColours)
    if (const int v = luigi ? luigiVar_ : marioVar_; v > 0) {
      uint16_t src[16];
      std::copy(out, out + 16, src);
      recolorCharacterPalette(src, out, v);
    }
  };
  if (row == 0) {
    // (The game's logic runs during the top of the next frame: at its row 0
    // what it drew last is what the OAM shows. With a frame's delay here his
    // tiles lagged his sprites, big Luigi's poses breaking up.)
    p2Shown_ = coop_ && p2Drawn_;
    std::copy(p2Tiles_, p2Tiles_ + 256, ppu.objAltTiles);
    ppu.objAltTileBase = p2TileBase_;
    ppu.objAltTileWords = p2TileWords_;
    if (p2Shown_) colours(!luigiLive_, p2_[varIndex(0x0756)], p2_[varIndex(0x07AF)], ppu.objAltColors);
    liveLuigiShown_ = coop_ && luigiLive_ && !lostLevels_;
    if (coop_ && lostLevels_) {
      // (The Lost Levels colours its player as the character he is, but
      // after a swap keeps the other's colours a moment (Mario in Luigi's):
      // his own while the palette is the other's, the game's after.)
      const int pal = (ram(0x0800 + ram(0x0B45) + 3) >> 1) & 7;
      const uint16_t* cg = &ppu.cgram[128 + pal * 16];
      for (uint32_t other : {luigiLive_ ? kMarioPal : kLuigiPal, luigiLive_ ? kMarioFire : kLuigiFire}) {
        bool same = true;
        for (int i = 1; i < 16 && same; i++)
          same = cg[i] == (uint16_t)(snes_.peek(other + 2 * i) | snes_.peek(other + 2 * i + 1) << 8);
        liveLuigiShown_ = liveLuigiShown_ || same;
      }
    }
    if (liveLuigiShown_) colours(luigiLive_, ram(0x0756), ram(0x07AF), ppu.objAlt2Colors);
    // (Only the entries that still hold the player's sprites, as drawn (tile
    // and attributes): the pause menu's box, drawn into them, took his
    // colours and tiles, dark patches across it.)
    auto held = [&](int p) {
      uint64_t bits = 0;
      for (int k = 0; k < 8; k++) {
        const int s = sprBase_[p] + k;
        if (s < 64 && ppu.oam[4 * s + 2] == sprSave_[p][k][2] && ppu.oam[4 * s + 3] == sprSave_[p][k][3])
          bits |= 1ull << s;
      }
      return bits;
    };
    altBits_ = p2Shown_ ? held(1) : 0;
    alt2Bits_ = liveLuigiShown_ ? held(0) : 0;
    // The level card (see coopPack_): Mario's head a head's width left,
    // Luigi's where it was (the same tiles, his colours), in free entries.
    // (Luigi's own head drawn by the game: the copy is Mario's, on the left,
    // in Mario's tiles and colours.) The game draws the head once and sends
    // its OAM copy each frame: done to what's sent (the PPU's), again
    // whenever it's sent afresh.
    const bool card = coop_ && snes_.frameCount() - cardFrame_ <= 1;
    if (card) {
      const int base = ram(0x0B45) / 4;
      if (base + 8 <= 128 && ppu.oam[4 * base] == ram(0x0800 + 4 * base)) {
        cardBits_[0] = cardBits_[1] = 0;
        auto hiBits = [&](int s) { return ppu.oam[512 + (s >> 2)] >> ((s & 3) * 2) & 3; };
        int at = 127;
        for (int k = 0; k < 8; k++) {
          const int s = base + k;
          uint8_t* e = &ppu.oam[4 * s];
          if (e[1] >= 0xE0) continue;
          while (at > base + 7 && ppu.oam[4 * at + 1] < 0xE0) at--;
          if (at <= base + 7) break;
          std::copy(e, e + 4, &ppu.oam[4 * at]);
          uint8_t& hi = ppu.oam[512 + (at >> 2)];
          hi = (uint8_t)((hi & ~(3 << ((at & 3) * 2))) | hiBits(s) << ((at & 3) * 2));
          ppu.objTrueX[at] = ppu.objTrueX[s];
          ppu.objTrueXSet[at] = ppu.objTrueXSet[s];
          cardBits_[at >> 6] |= 1ull << (at & 63);
          const int moved = cardLuigi_ ? at : s;  // (Mario's, to the left)
          ppu.oam[4 * moved] = (uint8_t)(ppu.oam[4 * moved] - 18);
          ppu.objTrueX[moved] = (int16_t)(ppu.objTrueX[moved] - 18);
          at--;
        }
      }
      altBits_ = alt2Bits_ = 0;
      if (cardLuigi_) {
        const uint32_t head = 0x0A9A00;  // (Mario's head)
        for (int i = 0; i < 128; i++)
          ppu.objAltTiles[i] = (uint16_t)(snes_.peek(head + 2 * i) | snes_.peek(head + 2 * i + 1) << 8);
        ppu.objAltTileBase = (uint16_t)(ram(0x0295) | ram(0x0296) << 8);
        ppu.objAltTileWords = 128;
        colours(false, 0, 0, ppu.objAltColors);
      } else {
        colours(true, 0, 0, ppu.objAlt2Colors);
      }
    }
    cardShown_[0] = card ? cardBits_[0] : 0;
    cardShown_[1] = card ? cardBits_[1] : 0;
    // (testing: each one's sprites drawn, and still in their entries as drawn)
    for (int p = 0; p < 2; p++) {
      const uint64_t h = held(p);
      heldNeed_[p] = heldHave_[p] = 0;
      for (int k = 0; k < 8; k++) {
        const int s = sprBase_[p] + k;
        if (sprSave_[p][k][1] >= 0xE0 || s >= 64) continue;
        heldNeed_[p]++;
        if (h >> s & 1) heldHave_[p]++;
      }
    }
  }
  ppu.objAltMask[0] |= altBits_;
  ppu.objAlt2Mask[0] |= alt2Bits_;
  (cardLuigi_ ? ppu.objAltMask : ppu.objAlt2Mask)[0] |= cardShown_[0];
  (cardLuigi_ ? ppu.objAltMask : ppu.objAlt2Mask)[1] |= cardShown_[1];
}
