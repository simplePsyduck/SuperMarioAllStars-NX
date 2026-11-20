# SMB1 / Lost Levels engine (All-Stars) — findings

Both titles share one engine port. SMB1 code: banks `$03-$05`; Lost Levels: `$0D-$10`.
The port locates everything by byte signature (`src/game/smb1.cpp`), so addresses
below are SMB1's unless noted.

## RAM (same for SMB1 and LL)
| addr | meaning |
|---|---|
| `$0770` / `$0772` | OperMode (0 title, 1 game, 2 ending, 3 game over) / OperMode_Task (3 = gameplay) |
| `$075A` | lives (displayed value - 1) |
| `$075F` `$075C` `$0760` | world, level, area number |
| `$0750` | area pointer; `$0751` entrance page, `$0752` alt entrance, `$075B` halfway page |
| `$0757` | fetch-new-timer flag, `$0774` disable-screen flag |
| `$0756` / `$0754` | power (0 small, 1 super, 2 fire) / size (0 big, 1 small) |
| `$07AF` | star timer (NES `$079F` + `$10`), `$07AE` injury timer |
| `$07E9-$07EB` | level timer digits |
| `$071A/$071B/$071C/$071D` | screen left page, right page, left x, right x |
| `$0725/$0726` | area parser page / column |
| `$0219,x` `$0078,x` `$0237,x` | object X, page, Y (x = 0 player, 1-6 enemies, ...) |
| `$00A0` | player Y speed |
| `$03AD-$03B3` | relative X per object class (player, enemy, fireball, ...) |
| `$0B45+` | per-object OAM offset (enemies at `$0B46,x`, base `$0900`) |
| `$0500` / `$05D0` | block buffer (collision), 2 pages of 16x13 metatiles |
| `$0800-$0A1F` | OAM buffer; `$0C00+4n` per-sprite high bits (bit0 = X bit 8), packed by `$05:DD7A` |
| `$1602` | music queue (e.g. `$0D` = star music) |
| `$5C` | BG3 mode (1 = cloud panorama streaming, only while `$0EDC` != 0: night levels have `$5C`=1, `$0EDC`=0) |
| `$0ED9` | next cloud metatile column index (`page*$E0 + col`, 12 pages, wraps at `$A80`) |
| `$7F3000` | cloud metatile map (14 rows x 16 cols per page) |
| `$7E2000` | hill panorama: 16 pages of 32x32 tilemap words (`$800` each, up to `$7E9FFF`); page = `$0EFE & $0F` (`$04:9B7D`). Streamed into BG2 in strips of 8 columns (one every 64 BG2 px). Night levels (3-1, 6-2) don't stream at all: BG2 is a 512 px map drawn once, shown as screen column c → map column c mod 64; the WRAM pages beyond the first two are stale. An area entered mid-level (a pipe's way out) has its pages written from its entry page on; earlier pages keep the previous area's (8-4). Verified 2026-10-21 with vramlog and WRAM/VRAM column fits. |

## Code
* Jumpspring area object `$03:ABD5` (LL `$0D:A967`): it takes the first free object slot (`FindEmptyEnemySlot` `$03:AA5E` searches 8 slots; LL's inline search covers 7), without checking that one was found. It then sets X `$021A`, page `$79`, Y `$0238` and `$5E` (fixed Y), ID `$32` in `$1C`, `$BC`=1 and flag `$10`, and writes metatiles `$6F`/`$70`. Slot 5 is also used by the flagpole's flag (`$30`) and by power-ups: a spring set up there is overwritten (2-1 at 21:9, with five enemies alive; the player then stood on its tiles, stuck). `EraseEnemyObject` is `$03:CE00` (LL `$0D:CD08`): it zeroes `$10,$1C,$29,$0110,$012E,$0792,$03A2` and sets `$0257`=`$20`. Verified 2026-10-21.
* Bowser (All-Stars): `BowserGfxHandler` (`$03:D647`) runs `ProcessBowserHalf` (`$03:D6D4`) for the
  front only. It moves the rear copy (`$06CF`), but the rear is never drawn. The whole of Bowser is six
  sprites drawn by `$05:D055` (LL `$0F` copy), called from `EnemyGfxHandler` at `$03:EF22`: four
  16x16 and two 8x8, X from `Enemy_Rel_XPos`. `$05:D14E` then sets X bit 8 in the `$0C00+4n` high
  bytes for each 8 px column past the 256 px screen, with a 16-bit position. It writes `$03` for the
  first column but `$01` (8x8) for the last column's two 16x16 sprites: hidden at 4:3, cut in half in a
  margin. Verified 2026-10-21 with watch/trace in 1-4.
* VictoryMode (`$0770` = 2) runs from the axe through the walk to Toad or the princess, the
  messages and the time tally. The area parser stops 96 px past the screen at 1-4's end.
  All-Stars' 8-4 ending writes its text on BG3 (map `$5800`); the Toad message is on BG1.
  In 8-4 the walk to the cage uploads no BG1 columns while the camera moves on about 277 px
  (parser `$0725` still advancing). The screen shows what the 512 px VRAM ring holds there.
  1-4's walk uploads its columns as usual.
* Vine (All-Stars):
  * The handler at `$03:BA36` (LL `$0D:B7F9`) works on the vine object, slot index `$039A,Y` (9 here), with flag count `$0398` and height `$0399`.
  * It draws the vine by `JSL $05:DA72`: X from `Enemy_Rel_XPos`, X bit 8 from a 16-bit position.
  * At `$03:BA6B` it erases the vine once `Enemy_OffscreenBits & $0F == $0F`, i.e. fully off the 4:3 screen.
* Castle maze loop (4-4, 7-4, 8-4): camera, parser and player go back exactly 1024 px.
  * The hills/backdrop layer (half speed) goes back its whole 512 px map, so on screen nothing changes. The panorama columns streamed from then on are those 64 columns further on.
  * The camera can briefly be left of 0 (4-4: 897 to -107). Level columns left of 0 are then real: VRAM still holds them.
* The title screen is `$0770` = 0, `$0772` = 3 (GameMenuRoutine). It shows 1-1's start with the
  logo; the attract demo plays there too.
* Cloud bonus areas (area pointer `$2B`; `$34` at night) have the "BONUS" Mario picture on BG2,
  written into the hill panorama pages, and plain sky after it.
* The card between levels (and TIME UP) is `$0772` = 1 (ScreenRoutines, `$073C` its step). Its grey
  is BG1 tiles filling map columns 0-31. The other 32 columns are empty, and BG2 keeps the last
  level's hills there. Text is on BG3, and the level picture is BG1 framed by BG2.
* The intro scene before x-2's pipe is area pointer `$29` (SMB1 worlds 1/2/4/7; LL 1/3/5/6), LL A/B `$3F`:
  a scroll-locked screen at camera 0, built out to x 384. The level select skips it.
* Castle step metatiles `$F5`/`$F6` are below `CheckForSolidMTiles`' group-3 threshold `$FC` (All-Stars' table, not the NES `$C4`), yet the player stands on them.
* `GetXOffscreenBits` `$03:FE1E` (screen-edge loads at `$FE22`, `$FE2B`).
* `CheckRightBounds` (enemy spawn at ScreenRight+`$30`) `$03:C4FA`; LL `$0D:C387`.
* `OffscreenBoundsCheck` (erase at left-`$48`, right+`$48`) `$03:DC09`; LL `$0D:DB06`.
* `GetBlockBufferAddr` `$03:ACF6` (table `$ACF2`), `BlockBufferCollision` `$03:EB81`.
* Area pointer lookup `$04:C034` (world tbl `$04:C11C`, area tbl `$04:C124`); LL `$0E:C389`
  (world tbl `$0E:C499`, 13 worlds, area tbl `$0E:C4A6`). Worlds with 5 areas have an
  intro area at index 1; the last area of each world has type bits `$60`.
* Cloud column builder `$04:8B87` (metatile table `$04:BD09`), hill column builder `$04:9B35`.
* NMI `$03:8308`; OAM DMA from `$0800` at `$03:9B1D`.

## Streaming limits (what the widescreen margins rely on)
* Level (BG1, 64x32 map at VRAM `$0000`): the parser runs 93-110px ahead of the right edge;
  the 512px ring leaves ~146px of history on the left. Margins: right <= 64, left <= 144.
* Hills (BG2, map `$0800`, 1/2 speed): 256px periodic, mirrored from the visible area.
* Clouds (BG3 rows 4-31, map `$5800`, 1/4 speed): built from `$7F3000` in the margins.
* HUD: BG3 rows 0-3 (scanlines 0-31), fixed scroll.

## Saves and fireballs (2026-11-19)

* **Game over menu** (`$03:A18D`, `$0F06` = 0 Continue / 1 Save & Continue / 2 Save & Quit):
  `STZ $075C` (`$A1A8`), `STZ $0760` (`$A1B4`), the world to SRAM `$70:0008` and level 0 to
  `$70:0009`, then the save `JSL $00:8F03` (for `$0F06` != 0).
* **Save** `$00:8F03`: the file at `$70:0010 + [$70:0004]` (`$120` bytes a file, one per All-Stars
  file, each game its own part): world, level, area, lives... written through `$00:92F6`
  (which sums them for the checksum written last). A world past the file's: world, then level 0
  (`$8F6E: LDA #$00`) and area 0 (`$8F73`). The file's own world or an earlier one: its own bytes
  again (`$8F54`-`$8F66`).
* **Load**: the menu copies the file to `$7FFB00` (`$00:A849`) and puts `$70:0008`/`$0009` (set
  from its file panel's world/level picker, `$B0,X`/`$C0,X` - 1) in `$7FFB00`/`$01`. The game
  takes world, level and area from `$7FFB00-02` (`$05:C95B`). The panel's picker takes a file's
  saved level only for The Lost Levels (`$00:A333`: `(Y & 3) == 1`); Super Mario Bros.'s start at
  level 1. The Lost Levels' own save keeps the level.
* **Fireballs**: ProcFireball_Bubble `$03:B680` (LL `$0D:B42F`), FireballObjCore `$03:B6E6` (LL
  `$0D:B495`), SprObjects 11 and 12. Per fireball: `$33,X` state, `$68` X speed, `$83` page,
  `$AB` Y speed, `$C6` Y high, `$020D` direction, `$0224` X, `$0242` Y, `$040C`/`$0427`/`$0447`,
  `$049A` box size, `$0FC8+4X` box; sprite offset `$0B56,X` (`$50`/`$54`), the burst's `$0B51,X`.
  OAM sprites `$01-$13` are never used by the game.
* **Saves**: after each castle (Continue / Save & Continue / Save & Quit, cursor `$0B77`, `$05:DF50`)
  and at a game over, both `JSL $00:8F03`; The Lost Levels' through `$00:90AC`. Neither keeps the
  power-up (`$0756`): a file starts small.
* **Pause menu** (Start while playing, `$03:8560`: Continue / Save & Continue / Save & Quit, `$05:DE31`
  sets it up, `$0776` paused, cursor `$0B77`, `$05:DF50` runs it): its saves are `JSL $00:8F03` too
  (`$05:DF95`, `$05:DFA4`), level 0 as at a game over. The Lost Levels' (`$0F:DB60`) keep the level.
* **Widescreen**: FireballObjCore's offscreen test (`AND #$FC` on `$03D2`): the low nibble is the
  sides, the high nibble ($F0) below or above the screen. Clearing all of it for a fireball in the
  wide view kept one that fell into a pit: its Y past the bottom, drawn wrapped at the top.
* **ForceInjury** (SMB1 `$03:DF52`, LL `$0D:DE8A`, as the NES): `LDX $0756; BEQ KillPlayer; STA $0756`
  (A = 0), `$07AE` = 8 (the injury timer: the blinking), sound `$1600` = 4, `JSL` GetPlayerColors, then
  `LDA #$0A` (+`$16`) ... `STA $0F` (the shrinking), out at +`$27` (`LDX $9E; RTS`).
* **Touching an enemy** (PlayerEnemyCollision, SMB1 `$03:DE83: LDX $9E; BCS $DE90`, LL `$0D:DDBE`): X the
  enemy; `[Enemy_CollisionBits]` `$0481,X` bit 0 marks a touch (one still marked from the last is left
  alone); a still shell (ID 0-3, `$29,X` & 7 >= 2, bit 7 clear) is kicked from `$03:DEE7` (LL `$0D:DE1C`):
  smack `$1600` = 3, `$29,X` | `$80`, EnemyFacePlayer, speed `$03:DE51,Y` (`$30`/`$D0`) to `$5E,X`, the
  points; out at `$03:DF14` (LL `$0D:DE49`). The shell's revival timer is `$07A2,X` (one step about
  every 20 frames). Enemy slots: flag `$10,X`, ID `$1C,X`, state `$29,X`, X `$021A,X` / `$79,X`, Y
  `$0238,X` / `$BC,X`, speeds `$5E,X` / `$A1,X`.
* **GameEngineSubroutine `$0F`**: 8 normal play, `$0A` shrinking (ForceInjury), `$0C` the fire flower's
  flashing (the game standing still, TimerControl `$0747` counting from `$FC` to `$C0`).
