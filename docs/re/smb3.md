# SMB3 (Super Mario All-Stars, USA) — reverse-engineering findings

SMB3 runs when `$7FFF00 == 8`. Code is in banks `$20-$3F`; most data tables are read with DB=`$21`.
NMI `$20:F000` (via `JSL $208003`), IRQ `$20:F304` (via `JSL $208006`), reset → `$20:A116` → top level `$20:8051`.
Game logic runs in the main thread: it spins in `$20:8044` waiting for `$0154` (a wait-for-NMI helper at `$20:801F`). The NMI does uploads only. The map main loop is at `$20:8452`, the level main loop at `$20:8D29`.

**NES layout carry-over (observed, useful with the NES SMB3 disassembly):** NES zero-page object vars `$75-$FF` sit **`$32` lower** (NES `Player_X $90` → `$5E`, `Player_Suit $ED` → `$BB`). Most of `$0100-$07FF` keeps its NES address (`$0553`, `$056E`, `$05EE`, `$0661`, `$0671`, `$0715`, `$0726`, `$0727`, `$0736`, `$0747`, `$070A`...). Some addresses moved, for example the P-meter is `$0413` (NES `$03DD`). NES SRAM tile memory `$6000` is at `$7E2000`, and the NES `$7D80` inventory is at `$7E1D80`.

All addresses are WRAM `$7E:xxxx` unless a bank is given (low WRAM is also reachable as `$00:xxxx`).
Verification was done with `re_B/smastool.exe`. The states named below are in `K:\projects\allstarspcport\re_B\`.
In the text, "poke" means I wrote the value and checked the effect with a screenshot or RAM read, and "trace" means I read the executed code in an instruction trace.

Controller mirrors use NES bit order: `$17` = held, `$18` = newly pressed. A=`$80`, B=`$40`, Select=`$20`, Start=`$10`, Up=`$08`, Down=`$04`, Left=`$02`, Right=`$01`. SNES Y maps to NES B (run) and SNES B maps to NES A (jump).
`$15` is the frame counter.

---
## 1. State detection

| addr | meaning | verified |
|---|---|---|
| `$0100` | NMI mode. `$20` title/menu; `$C0` world map, normal horizontal level, toad house, king's room, battle; `$80` vertical level; `$40` spade game; `$95` airship; `$00` briefly while a level loads (`$20:8544`). Dispatch is at `$20:F033`. | peeked in every state below |
| `$0101` | IRQ/raster mode. `$20` title; `$00` normal status-bar split (map and levels); `$60` spade sliders; `$95` airship cloud bands; `$80` no split (battle arena). Other handlers exist for `$40/$A0/$55/$75` (dispatch `$20:F312`). | peeked |
| `$070A` | **Level tileset**. `0` = world map (and the title). Levels use `1` plains, `2` fortress/king's room, `3` hills, `4` sky, `5` plant, `6` water, `7` toad house, `8` vertical pipe maze, `9` desert, `$0A` airship, `$0B` giant, `$0C` ice, `$0D` clouds, `$0E` underground, `$0F` spade game, `$12` 2P battle arena. `$10`/`$11` follow the NES meaning (N-spade / vs.) and are **unverified**. | observed 0,1,2,6,7,8,`$0A`,`$0F`,`$12` |
| `$0425` | vertical level flag (`$10` = vertical; header byte 8 bit 4, set at `$20:99DF`) | 7-1 pipe maze = `$10` |
| `$037A` | pause flag (≠0 = paused; the level loop then runs the pause menu `$29:EBF7`, see `$20:8E60`) | poke 1 freezes gameplay |
| `$00BF` | player dying (≠0; 3 = time-up death) | diff at death |
| `$009C` | game halted (dying / power-up change / etc.; built at `$23:C030`) | code |
| `$0729` | world-map sub-state (`$0D` idle on a panel, `$10` game-over panel / inside level, `$0B` after dying) | diffs |
| `$0736+p` | lives of player p. `$FF` means game over (the map then shows the GAME OVER / CONTINUE panel). | timer-death with lives 0 |
| `$00AC` | title-screen state (3 = 1P/2P/Battle menu, 7 = hidden world select, see §3) | peek/poke |
| `$072B` | title menu choice / game type (0 = 1P, 1 = 2P, 2 = battle on the menu; 3 after battle starts). Once a game starts it is the number of players (1/2): returning from a level, `$20:817B` copies each player's map position (`$43/$45/$47/$52`) back from `$7E3975+` that many times. Left at 0, the player came back at 0,0 off the paths, and the level wasn't marked. | menu |
| `$001D` | ≠0 in 2P battle (`$20:86C4`) | battle = 2 |

Recommended tests:
* **Title**: `$0100==$20`.
* **World map**: `$070A==0 && $0100==$C0`. Game over: `$0736[$0726]==$FF`.
* **Gameplay**: `$070A` in `1..$0E` (`$0A` airship) and `$0100` in `{$C0,$80,$95}`. Vertical: `$0425!=0`.
  * 1-1, 1-3, 1-4 (auto-scroll), 1-5, water, fortress and king's room all showed `$C0/$00`. Only the airship showed `$95/$95`.
  * The `$55/$75` raster modes were not seen.
* **Toad house**: `$070A==7`. **Spade**: `$0F`. **Battle**: `$12` with `$1D!=0`.
* **Pause**: `$037A!=0`.
* **Ending**: not reached, **unverified**.

World and position:

| addr | meaning |
|---|---|
| `$0727` | world index 0-7 (8 = warp zone "World 9"). The status bar shows `$0727+1`. |
| `$0726` | current player (0 = Mario, 1 = Luigi; used as an index everywhere) |
| `$0043+p` / `$0045+p` / `$0047+p` | map Y, map screen (XHi), map X (Mario p=0, Luigi p=1). Panel grid is 16 px; X and Y equal screen pixels when XHi = scroll screen. Verified: poking moves the map sprite. |
| `$7E3975/77/79 +p` | saved map Y/XHi/X per player |
| `$0722` / `$0724` (+p presumably) | map camera (`$0210`/`$0211`) saved on entering a level; the map comes back to it (tested: poking `$0722=0` or `$0724=1` in a level moved the map's camera on return). The map scrolls in half screens (`$0210` went 0→`$80` as Mario walked to X `$E0` on World 3). Level select sets the camera before entering, or a panel on another map screen came back with the map showing the start screen and Mario off the paths. |
| `$2B-$2D` | 24-bit level layout pointer (loader). `$1EBB-$1EBD` keeps a copy of the current one. |
| `$31-$33` | 24-bit level object-list pointer (`$023A-$023B` copy) |
| `$0022` | level width in screens − 1 (header) |

Power-up (see §2): `$00BB` in level, `$0747+p` on the map.

---
## 2. Cheat addresses (tested by poke unless the test column says "code")

| what | addr | encoding / effect | test |
|---|---|---|---|
| lives | `$0736` Mario, `$0737` Luigi | shown value = stored+1 (`$09` shows "10"); `$FF` = game over | poke `$09` gave "10" on the HUD |
| suit (in level) | `$00BB` | 0 small, 1 super, 2 fire, 3 raccoon, 4 frog, 5 tanooki, 6 hammer | poked 0-6: sprite shape changes. **Palette is not refreshed**; the palette loader is `$23:C467` (an RTS routine that picks a palette by suit/player and block-copies 16 colours to the CGRAM buffer, probably `$14E0`; the destination is inferred from the code). |
| suit (map/next level) | `$0747` Mario, `$0748` Luigi | same encoding, copied to `$BB` with the correct palette at level start (`$23:C0DE`) | `$0747=2` on the map, then entering 1-1 gives a correct fire Mario |
| star | `$0553` | invincibility timer, counts down 1 per 2 frames (`$FF` ≈ 8.5 s) | goomba killed on contact, star palette |
| other player timers | `$0551` grow anim, `$0552` hurt-blink, `$0554` transform puff | | screenshots |
| P-meter | `$0413` | bits 0-5 = arrows, bit 6 = P (`$7F` = full). The game decays it. | poke `$07` = 3 arrows, `$7F` = full |
| flight | `$056E` | fly timer, decrements every 2 frames while flying. **`$FF` = P-wing: never decrements = infinite flight** (needs suit 3 or 5; P-meter not required) | `$FF` stayed `$FF` for 320 frames of flying (Mario reached Y<0); `$80` counted down 16 per 32 frames |
| P-wing from map | `$0429==8` at level start sets `$0413=$7F`, `$056E=$FF` (`$23:C0F7`) | | code |
| level timer | `$05EE/$05EF/$05F0` | hundreds/tens/ones digits (0-9). `$05F1` = frame sub-counter (reload `$28` = 40 frames per unit). Timer stops while `$058C!=0` or `$05F3&$7F`. Tick code `$29:E5FE`. | poke 1,2,3 shows "123" |
| score | `$0715-$0717` | 24-bit big-endian **score/10** (HUD adds the trailing 0) | `$01E240` shows "1234560" |
| coins | `$1DA2` (Luigi `$1DC5` by layout) | binary 0-99. The HUD refreshes on the next coin. | poke `$29`, then a coin block gives "42" |
| world-map items | `$1D80-$1D9B` Mario (28 slots, 7 per row), Luigi `$1DA3-$1DBE` (offset `$23`, `$20:F5D1`) | 0 empty, 1 mushroom, 2 flower, 3 leaf, 4 frog, 5 tanooki, 6 hammer suit, 7 cloud, 8 P-wing, 9 star, `$0A` anchor, `$0B` hammer, `$0C` warp whistle, `$0D` music box | poked 1-13 and opened the item bar with X/Y on the map |
| end-level cards | `$1D9C-$1D9E` (Luigi `$1DBF-$1DC1`) | 0 none, 1 mushroom, 2 flower, 3 star (BG tiles in the HUD) | poked, visible after the next HUD redraw |
| saved score slot | `$1D9F-$1DA1` (Luigi `$1DC2`) | copied to `$0715` when the map starts (`$20:8162`) | code |
| pause | `$037A` | see §1 | |

---
## 3. Level select

**A. Warp to any world map (tested for W3, W5, W7, W8).**
Use state `s_after_start.state`: title menu, `$00AC==3`, "1 PLAYER GAME" selected.
1. Press Start (2 frames).
2. Within the next ~30 frames write `$7E0727 = world-1` (0-7). The map init reads it ~36 frames after Start (`$29:CC7C`), and nothing overwrites it before then.
3. The "WORLD n" banner and map of that world appear; Mario is at START. The map is idle after ~400 frames.

Side effects: none seen (lives, items and score are the new-game defaults).

Alternative: a hidden debug world select. Its menu state is `$20:B943`. In the menu handler `$20:B79E`, the check `$F3 & $C0 == $C0` now branches into 5 NOPs at `$20:B7A6-$20:B7AA` (the action was removed). No retail code sets `$AC=7`.
To use it anyway: on the menu write `$00AC=7`, write `$03FE = world-1` (Up/Down also change it), then press Start. The game starts in that world (tested: W5). The cursor sprite is not visible.

**B. Enter a specific map panel (tested: 1-3, W5 fortress, 7-1, W8 fortress on map screen 2, W1 toad house, spade, king's room).**
Do this on an idle map (`$0729==$0D`, after recipe A if another world is needed).
1. Write `$7E0043 = Y`, `$7E0045 = XHi`, `$7E0047 = X` (+1 for Luigi).
2. Run 2 frames.
3. Press A for 4 frames.
4. Wait about 300 frames.

The lookup `$29:BF84` (called from `$20:86F7`) uses per-world tables in bank `$21`:
* `$21:D86B` screen start index
* `$21:D87D` rowtype (Y & `$F0`, low nibble = tileset)
* `$21:D88F` screen/col (`XHi<<4 | X>>4`)
* `$21:D8A1` object ptrs, 3 bytes each
* `$21:D8B3` layout ptrs, 3 bytes each

The full dump is in `re_B/maplevels.txt`. W1 examples, as (Y,XHi,X):
* 1-1 (`$20`,0,`$40`), 1-2 (`$20`,0,`$80`), 1-3 (`$20`,0,`$A0`), 1-4 (`$40`,0,`$A0`), 1-5 (`$A0`,0,`$40`), 1-6 (`$A0`,0,`$80`)
* fortress (`$60`,0,`$60`), king's room (`$80`,0,`$C0`), toad houses (`$40`,0,`$C0`) and (`$80`,0,`$60`), spade (`$60`,0,`$80`)

Examples in other worlds:
* W5 fortress: `04 40 00 C0`, screenshot `w5f_lvl.png`
* 7-1: `06 50 00 20`
* W8 fortress: `07 70 02 80`, screenshot `w8f_lvl.png`

Side effects: the map sprite jumps to that spot. Clearing the level marks that panel as done. The map view is not re-scrolled when XHi differs from the current screen, but the level still loads. The path from the new spot back to the map was not tested.

**C. Load any level/area directly, including airships and pipe sub-areas (tested: W1 airship, 7-1 vertical pipe maze).**
Start as in B: press A on any panel. After `$29:BF84` has run and before the header is parsed 2 frames later, overwrite:
* `$2B-$2D` = layout pointer
* `$31-$33` = object pointer
* `$070A` = tileset (it must match, or the game crashes)

In a port, hook right after `JSL $29BF84` at `$20:86FB`.
With the tool: from `s_map_on11.state`, `press a 4 0; run 103`, then poke. The timing varies per world (W7 needed 73 frames), so watch `$2B` change.

Pointer tables:
* Airship per world: layout `$21:D7DD+3w`, objects `$21:D7F5+3w`, tileset `$0A` (`$29:C0E9`). W1 = `$26BA4F`/`$27FC97` (screenshot `air1.png`).
* King's room: `$21:D7C2+3w`, tileset 2.
* A level's alternate area (pipe or door destination) is in its header, see below. Example: 7-1 main `$248F75` → vertical area `$258FF0` / objects `$27F343`, tileset 8 (`vdirect.png`).
* In-level junction trigger: writing `$0414=1` jumps to the current header's alternate area. The screen stays black, because the fade state is skipped; writing `$0016=$0F` fixes it.

Level header (11 bytes at the layout pointer, parsed at `$20:991A`):
* bytes 0-2: alternate layout pointer
* bytes 3-5: alternate object pointer
* byte 6: bits 7-5 start-Y index (tables `$21:CE2A/CE32/CE3A`), bits 3-0 width in screens − 1 (→ `$22`)
* byte 7: → `$073B/$073C/$0426/$0562`
* byte 8: bit 4 vertical, bits 3-0 → `$1EBA`
* byte 9: → `$0545/$1EBF`
* byte 10: bits 7-6 timer (`$21:CE42`), bits 3-0 music

---
## 4. Rendering (widescreen)

**Layers in a level.** BG mode 1 with BG3 priority (`$2105=$09`).
* **BG1** = playfield **and** status bar. Map `$0000`, 64x64 (512x512 px), chr `$2000`.
* **BG2** = background scenery. Map `$1000`, 64x64, chr `$2000`. It is a static 512-px-wide repeating pattern, not streamed in the levels checked (1-1, W5 fortress, airship). hofs `$0212/$0213`, vofs `$0218/$0219`. In plains, hofs = cam/2 (`$0212` holds the value) and vofs = `$0216` − `$0216`/4 (set at level load, `$20:996A`). Other background types (`$02BF`, `$0350`) use other rules (`$20:FCDF`-`$20:FEED`).
* **BG3** = clouds. Map `$5000`, 32x64, so it repeats every 256 px. In plains (`$02BF=2`, `$20:FE4A`): hofs = camX/4 (also stored in `$0353/$0354`), vofs = camY − camY/8 + `$20`. Checked at camX=`$402`: BG2 hofs `$201`, BG3 hofs `$100`.
* Screens: 1-1 has TM=`$15` (BG1, BG3, OBJ) and TS=`$13` (BG1, BG2, OBJ), CGWSEL=`$02`, CGADSUB=`$20`. BG2 shows only through the backdrop via colour math. Each tileset has its own shadow values `$0203-$0209` (CGWSEL, CGADSUB, COLDATA×3, TM, TS), written by `$20:F20B`.
* No windows and no HDMA in normal gameplay (`$2123-$2125=0`, TMW=0, HDMA mask `$0293=0`). There is **no NES-style left-column mask**.

**Scroll / camera.** The NMI writes BG1 scroll at `$20:FCBF`.
* **Camera X** = `$0210` (lo) / `$0211` (hi, full value) → BG1HOFS. `$0012` is a copy of the hi byte (NES `Horz_Scroll_Hi`), used by the streaming, spawn and delete code.
* **Camera Y** = `$0216` (lo) / `$0217` (hi) → BG1VOFS, plus the 8-bit shake offset `$1CF2`. `$0542/$0543` = hi/lo copy used by object code.
* Horizontal levels: camY is 0..`$EF`. Screen row r shows level y = camY+1+r. The level is 432 px (27 rows) tall, so camY=`$EF` shows the bottom.
* Tilemap coordinates equal level coordinates mod 512 (BG1 x = level x mod 512, y = level y).

**Status-bar split (IRQ).** The NMI sets VTIME=`$C0` (`$20:F098`). The IRQ (`$0101==0`, `$20:F34D`) fires at **V=192** and does the following:
1. Waits for H-blank.
2. Sets `INIDISP=$80` (force blank) and disables HDMA.
3. Clears W12SEL/W34SEL/TMW.
4. Sets BGMODE=`$020E` (`$01`, no BG3 priority), TM=`$020F` (`$01` BG1 only in levels, `$11` BG1+OBJ on the map), CGADSUB=`$20`, BG1HOFS=`$0100`, BG1VOFS=`$0100`.
5. At the next H-blank restores brightness from `$0016`.

The result in screenshot rows (0-223):
* **rows 0-191**: playfield (192 lines)
* **rows 192-193**: black (193 is force-blanked)
* **rows 194-219**: status bar graphics, which is BG1 tilemap region x 256-511, y 448-479
* **rows 220-223**: black
* No sprites can appear under the split in levels (TM=BG1).

Airship (`$0101=$95`, `$20:F528`) chains extra IRQs at V=145/158/165/172 that change BG3HOFS (cloud bands), then does the status split at 192 without force blank. Spade game uses `$0101=$60`.

**Column streaming (horizontal levels).**
1. `$20:8E70` computes `$24 = ((camX>>4)−1)` and `$23 = ((camX>>4)+$11)` as 8-bit `screen<<4|col` indexes (`$20:97F5` plus `DEC $24 / INC $23` at `$20:8E96`).
2. `$20:9D5A` (JSL from `$20:8F25`) compares `camX & $F8` with `$26` (moving right) / `$27` (moving left), direction in `$25`. When it changes, it renders one **8-px-wide, full-height** column: the left or right half of metatile column `$23` (or `$24`), all 27 metatile rows.
3. The column goes into the buffer `$0380-$03ED`:
   * `$0380/$0381` = VRAM address, big-endian; `$0380=$FF` means empty
   * `$0382-$03C1` = 32 words (tilemap rows 0-31)
   * `$03C2-$03ED` = 22 words (rows 32-53)
4. The NMI uploads it at `$29:E953` (VMAIN `$81`, 2 DMAs to `$0400+col` / `$0C00+col` etc.).
5. At level start 34 columns (x 0..271) are drawn.

Measured by re-rendering the whole BG1 tilemap from level RAM and comparing it with VRAM at display time (`re_B/verify_cols.py`):

| direction | valid tiles ahead of the screen edge | valid tiles behind |
|---|---|---|
| scrolling right | **12-19 px** past the right edge (column `(camX&~7)+272` is drawn once `camX&7 ≥ 2`) | ≈238-243 px to the left (the rest of the 512-px ring, as long as that area was visited) |
| scrolling left | **10-19 px** past the left edge (column `(camX&~7)−16`) | ≈237-246 px on the right (the rest of the ring) |

The two sides always add up to about 512−256 px, so one side has a lot of history and the other almost none. Measured at camX 555-1039 in 1-1 (`s_fly2`) and at 0-210.

* A column is drawn only when the camera lands 2-7 px into a new 8-px block (moving right; 0-4 moving left). A camera jump of 7 px or more in one frame therefore skips a column, and the ring's old content stays on screen there. Normal play never moves the camera that fast. The QA sweep can: it pokes Mario forward while he is blocked (speed 0), and the camera then catches up 8 px at once (w2_7 at camX 2560 leaves x 2832 stale).
* **Tile changes** made by the game logic (a coin collected, a block a Buster Beetle carries off, a block hit) go into `$7E2000` at once, but reach VRAM only at the next NMI. Tile memory as it was at vblank is what the tilemap shows for the following frame.
* The game's replacement words can differ from the metatile table in bits that draw nothing. For example, a collected coin (`$40` → `$41`, tileset 9) is blanked in VRAM with `$0005-$0008`, while the table's `$41` is `$1405-$1408`: the same transparent tiles in another palette.

Vertically, horizontal levels always hold the whole 27-row height. Tilemap y 432-511 is blank (`$00FF`), except the status bar at x 256-511, y 448-479.

**Level tile grid in RAM.**
* **Tile memory `$7E2000`**, horizontal levels: screen s (0-14) at `$2000 + s*$1B0`, 16 columns × 27 rows, `offset = s*$1B0 + row*16 + col`. The area ends at `$7E394F`.
* **A second "high page" `$7E4000+offset`**: if it is non-zero, the metatile number gets +`$100`. The renderer adds `$800` to the table offset (`$20:9E39`).
* Vertical levels: pages of 16 cols × 15 rows (`$F0` bytes, 240 px) stacked downward: `offset = (y/240)*$F0 + ((y%240)/16)*16 + col`. Verified by re-render in the 7-1 pipe maze.
* The **world map uses the same store**: map rows 17-25 of each 16-column screen, all 4 screens resident.

**Metatile → tilemap words.**
* Table address = `bank($21:CE80+ts) : word($21:CE5A+2*ts)`, 8 bytes per metatile: `+0` top-left, `+2` bottom-left, `+4` top-right, `+6` bottom-right (SNES tilemap words, palette and priority included).
* Tables: 0 `$21:CF56` (map), 1 `$24:8000`, 2 `$26:8000`, 3/14 `$24:A480`, 4/12 `$24:DE00`, 5/11/13 `$25:A800`, 6/7/8 `$25:8000`, 9 `$25:CC00`, 10 `$26:A800`, 15-17 `$21:8E3A`, 18 `$21:9B79`.
* Verified by recomputing every column in 1-1 (tileset 1), 64/64 matched in valid range, and the W8 map (tileset 0) and 7-1 vertical rows (tileset 8) matched too.
* A port can therefore render any column or row itself from `$7E2000`/`$7E4000` plus these tables.

**Vertical levels.**
* BG1/BG2 use only the **left 256 px** of the 64x64 map. The right half is empty, so widescreen margins show nothing; mask or pillarbox them.
* Rows of 32 words (left screen only) are streamed ahead of the scroll. Scrolling up, row `(camY&~7)−16` is written, which gives about 16-23 px above the top. Scrolling down, the lead is roughly 8-24 px below the bottom (camY+193). The down-scroll figure is rough (vramlog camera sampling is one frame late).
* A re-render check at camY=1680 found valid rows from 33 px above to 23 px below the visible area.
* `$0100=$80`; row upload runs from the NMI `$20:F0CF` path.

**World map.**
* BG1 holds only the **currently shown map screen** (tilemap x 0-255, y 272-415). BG2 holds the decorative frame (a static 256-px screen). TM=`$13`.
* The whole map (up to 4 screens) is in `$7E2000`, and tileset-0 metatiles render it exactly. A wider map view is therefore possible by rendering neighbouring columns, but the frame, the black surround and the map-scroll logic are all built for 256 px. Recommend keeping it 4:3, or extending it by rendering from RAM.
* Map camera = `$0210/$12`.

---
## 5. Objects and the screen edge

**Object slots** (index x = 0-7; spawning uses 0-4; NES layout shifted):
* `$0661+x` state (0 empty, 1 init, 2 normal, 6 killed...)
* `$0671+x` ID (e.g. `$72` goomba)
* `$005F+x` X lo, `$0044+x` X hi
* `$0071+x` Y lo, `$0056+x` Y hi
* `$008C+x` X velocity, `$009E+x` Y velocity
* `$007A+x`/`$0083+x` sprite X/Y (= pos − camera, low byte; `$27:9DC2`)
* `$0651+x` horizontal visibility bits (bit 7 = first 8-px column ... set = column outside [0,256))
* `$0681+x` vertical visibility bits
* `$0659+x` index in the level object list
* `$7FC532..` SNES-side copies

**Player:**
* `$005E` X, `$0043` X hi, `$074E` X fraction
* `$0070` Y, `$0055` Y hi
* `$008B` X velocity, `$009D` Y velocity
* `$0079`/`$0082` sprite X/Y
* `$00A6` in-air

**Level object list:** `$1B40` has 3 bytes per object:
* byte 0: ID
* byte 1: `screen<<4 | col`
* byte 2: `YHi<<4 | Yrow`

`$1B00` holds the first list index per screen, and `$1B10` holds per-object flags (bit 7 = already spawned; cleared on delete so it can respawn).

**Spawn** (`$29:9AE0`, reached from `$27:9009` → `$29:9A9A`; horizontal levels):
* Spawn column = `(cam + T) & ~15`, with T from `$21:C699/$21:C69B` (lo/hi, index `$25`):
  * **right `+$0110` (272)**: objects appear with their left edge 1-16 px past the right edge
  * **left `−$0020`**: 17-32 px past the left edge
* Only the exact matching column is checked each frame.
* Vertical levels (`$29:9DCD`, camY `$0542/$0543`): `$21:C6A9/$21:C6AB` = `−$20` / `+$120`.
* **To extend by N px:**
  * right = `$0110+N` → `$21:C699` (lo), `$21:C69B` (hi)
  * left = `$FFE0−N` → `$21:C69A`, `$21:C69C`
  * `$29:A1F7` also uses `C699−C69A`, so patch both.

**Delete** (`$27:9BDF`):
* An object is only considered when it is not fully visible (`$0651|$0681 != 0`), every other frame, one side per check.
* Horizontal: delete if X > cam + R or X ≤ cam + L. Table lo `$21:AFE7+i`, hi `$21:AFED+i`:
  * i=0/1 normal (entry `JSL $279BC3`): **R=+`$0120` (288), L=−`$0030` (−48)**, confirmed by probing a goomba
  * i=2/3 wide (entry `$27:9BC7`): +`$0180` / −`$0080`
  * i=4/5 (entry `$27:9BCB`): +`$0140` / −`$0050`
* Also deleted when Y hi ≥ 2 (`$27:9BFB`).
* Vertical levels: `$21:AFE3/$21:AFE5` = +`$0140` / −`$0050` relative to camY (`$27:9C65`).
* **To extend by N:** add N to the three "right" words and subtract N from the three "left" words.

**Sprite X / hiding.**
* Objects: the draw routine (e.g. goomba `$22:CBA0`) takes OAM X lo from `$7A+x` (+8 per column). The **X bit 8 is the `$0651` visibility bit** (`$22:CCA2`): `ROL` into the per-sprite high byte `$0A20+slot`.
* **Object sprites are therefore never hidden horizontally.** They are emitted with 9-bit X = (x−cam) mod 512. Verified: a goomba at cam−64 appears in OAM at X=448, at cam+272 at X=272.
* Only vertically invisible objects are skipped (`$22:CBB6`: `$0681` bit 0).
* The visibility test itself is `$27:A2AF` (`SBC $0210 / CMP #$0100`).
* OAM buffer: `$0800-$09FF`. The per-sprite high/size bytes are at `$0A20-$0A9F`, packed to `$0A00-$0A1F` by the NMI at `$20:F951`, then DMA from `$0800` (`$20:F9AA`).
* Mario uses 9-bit X too (16-bit `PlayerX−$0210` at `$20:E24B`).
* A widescreen renderer must read 9-bit X in 256..256+E as the right margin and 512−E..511 as the left margin.

**Other edge checks** that hide or delete at the 256-px screen (patch for margins):
* player fireballs `$27:B514` `CMP #$0100` / `$27:B519` `CMP #$FFF8`: deleted outside [−8,256); state `$1CE1+x`, X `$1CE5+x`
* special objects (`$05C9+x`/`$E2+x`) `$27:C587` `CMP #$00F8` / `$27:C58C` `CMP #$FFF8`: [−8,248)
* objects at `$1F96+x`/`$EF+x` `$27:BD38` / `$27:BD3D`: [−16,256)
* burner flames' pieces (airships, `$B1` etc.), `$29:89AB`: `CMP #$0100` at `$29:89D6`, `CMP #$FFE0` at `$29:89DB` (16-bit): a piece is drawn only in [−32,256). Unpatched, a flame reaching into a margin stopped at the 4:3 edge, and a burner in a margin had none.
* `$29:8A30` `CMP #$0140`: |x−cam| < 320
* In total, **97 code sites subtract `$0210`**; use the byte pattern `ED 10 02` to audit (list printed in the investigation).

**Slots 5-7** are not free for level objects:
* 5: items an object releases (`$28:91B8`, `$28:B120` write slot 5 without checking it is free);
* 6-7: power-ups from blocks (`$057C` pending, `$29:9AA6`; lost if both are taken) and level object `$41` (`$29:9BBB`).
The object loop (`$27:9017`) updates all eight; the level spawner only uses 0-4.

**Five slots in a wide view** (QA 2026-10-21, `K:/smas_qa/scan/eval3.py`, s3qa sweep at 21:9):
* With the wide delete range, objects the 4:3 game has deleted stay alive behind the player
  (up to 48 px beyond the view's edge). A spawn at the wide edge then often found no free slot.
  The retry waited for two free slots, or for the 4:3 spawn point, so the object appeared about 100 px inside
  the view. There were 151 such pop-ins in 57 areas.
* Freeing a slot from an object out of sight (deleted by the 4:3 game, or a wide-only one further
  out) and letting the object about to appear take the last free slot gave 91 pop-ins (31 deep,
  ≥ 96 px). Objects vanishing in view also fell, from 59 to 50.
* Also freeing slots from objects still visible in the margin behind gave 67 pop-ins (11 deep),
  but 135 vanishes. Rejected.
* Left over:
  * the tank levels: the 4:3 game itself holds all five slots;
  * World 7's pipe-plant level: plants visible in the left margin hold them;
  * 5-9: while `$05FE` is set, the camera jumps about 225 px in 56 frames carrying everything on
    screen, and the wide range rests. The margin's objects come in just after the jump. (Tried 2026-10-26:
    spawning ahead during the jump, uncarried until the 4:3 spawn reach. The jump also wraps camera Y, so early
    objects were left below the screen and deleted, and platforms went missing. Reverted.)

**Spawn routine details** (`$29:9AE0`): walks the page's list from `$1B00[page]` (entry i: ID `$1B41+3i`,
column `$1B42+3i`, row `$1B43+3i`). Special IDs (`$B4`+, `$D1-$D6`) are handled and marked spawned in
the same call, but it spawns **one regular object per call** (success ends at `$29:9C3F`,
`INC $0661,X`; Y = list index, `$05/$06` = column). A column checked only once (the level-load sweep,
a jump of the range) leaves a second object in it unspawned.

**Delete range per object:** at `$27:9BEE` (`STA $00`) A = 0/2/4, the range used (normal / wide /
`$0140` variant, see the table above); `$1FF1,X` switches some objects to the wide range.

**Auto-scroll:**
* Bowser's palette: every level starts with the default sprite palettes (`$3C:8800` -> `$1400`, sprite palette 4 = `$3C:8880`, Bowser with a blue shell). `$29:CBC0` loads his own (`$3C:8A20` -> `$1480`, green shell) only when `$0727` = 7, `$070A` = 2 and `$7E3977` + player (the map position's screen, saved by `$29:C19E` as he enters a level) = 3, the castle's place on the World 8 map. The castle's areas have objects `$27FB7D` / `$27FBBB`.
* `$1EB8` != 0: the screen is held still. The camera routine (`$23:CFCA`) skips following the player at `$23:CFD2`, and he is kept on screen. Set by Boom-Boom's appearance (`$28:D91D`, with boss music `$12`) and by Bowser's room (`$29:F897`); toggled at `$29:E06A` (8-1's one-screen underground rooms are held this way, the camera at `$0400`); cleared at `$20:874A`.
* `$0580` = auto-scroll on (gates `JSL $278500` at `$20:8F4E`); stays set through pauses.
* `$1A01` = mode (jump table at `$27:8526`: 0 horizontal script, 2 diagonal (5-9)); `$1A02` script,
  `$1A03` position, `$1A08` timer, `$1A09` 4-frame sub-counter, `$1A0E` X speed (4.4, cruise `$08`),
  `$1A0F` Y speed, `$1A10`/`$1A11` fractions.
* At a level start the speed picks up by 1 every 4 frames (`$1A09`) to `$08`.
* `$05FE` != 0: objects are carried with the camera (`$27:A994` adds `$7E3952`/`$7E3953` to X/Y): 5-9's
  camera jumps ahead (4 px a frame, camera Y wrapping) with the screen's contents.

**Level entry:** the level is built while the screen is black (NMI mode `$C0`), then fades in and
waits in NMI mode `$00`. The opening circle is HDMA window 1 centred on (128, 127), radius
`$0281` - 10 on the next frame (+8 a frame); the window goes off (TMW `$00`) once it covers 256 px.

**Background scroll** is written in the NMI (`$20:FBC5`...) in several ways per level (`$02BF`,
`$0350`): BG2 from `$0212/$0213` or `$0353`, BG3 from `$0214/$0215`, or camera >> 1/2/3, with HDMA
bands in some levels; the same tileset can use different speeds (e.g. tileset `$0A`).

---
## 6. HUD / status bar

* Layer BG1 (the same layer as the playfield), shown by the V=192 IRQ with scroll (256,256).
* VRAM: map words `$0F00-$0F7F` = rows 24-27 of the lower-right 32x32 quadrant.
* Screen: rows 194-219 visible (tile rows 24-27 → screen y 192-223), x = 8-247 (tile cols 1-30). Main panel cols 1-20 (x 8-167), card panel cols 21-30 (x 168-247). Cols 0 and 31 are filler (`$22AE`).
* Tile words use palette 0 with priority; digit tile = `$2290+d`.

| element | tilemap row / col | screen x |
|---|---|---|
| "WORLD" | row 25, cols 2-5 | 16-47 |
| world digit | row 25, col 6 | 48 |
| P-meter arrows | row 25, cols 8-13 | 64-111 |
| "P" badge | row 25, cols 14-15 | 112-127 |
| coin icon + 2 digits | row 25, cols 17 / 18-19 | 136 / 144-159 |
| M/L badge, "x", lives (2 digits) | row 26, cols 2-3, 4, 5-6 | 16-55 |
| score (7 digits) | row 26, cols 8-14 | 64-119 |
| clock + timer (3 digits) | row 26, cols 16 / 17-19 | 128 / 136-159 |
| cards 1/2/3 | rows 25-26, cols 22-23 / 25-26 / 28-29 | 176 / 200 / 224 (16x16 each) |
| borders | rows 24 and 27 | |

* The HUD is redrawn **every 2 frames** through the VRAM update buffer `$7E1600`: entries `[addrHi][addrLo][00][bytes−1][words...]`. It rewrites `$0F28` (12 words, row 25 cols 8-19) and `$0F45` (15 words, row 26 cols 5-19). The builder is `$29:EA69` (from NMI), the uploader `$29:E8AB`.
* Nothing in the level HUD is a sprite: TM=BG1 during the split, and the cards are BG tiles.
* On the world map, TM=`$11`, so sprites can overlap the HUD rows. X/Y on the map replaces the HUD with the item bar.
* In widescreen the HUD rows would show BG1 x 256−E.. and 512..512+E (wraps to x 0..E, y 448-479), which is blank (`$00FF`) in levels. Re-anchoring needs the port to redraw that band (read VRAM `$0F00-$0F7F`) at its own x offset.

---
## 7. Things a widescreen port must handle

* **Playfield is 192 lines**, not 224. Row 192/193 black, HUD below (BG1, forced-blank split at V=192).
* **The leading edge has only about 10-19 px of valid tiles** (the right edge when moving right, the left edge when moving left). Anything wider needs extra columns: either change the lookahead at `$20:9807` (`ADC #$10`) and `$20:8E96`, or render from `$7E2000` with the tables in §4. The 512-px ring limits total width (visible + both margins + 8) to ≤ 512 px.
* **Objects spawn 1-16 px beyond the right edge and are deleted at +288/−48.** Fireballs vanish at the edge and special objects at 248. Patch the tables and constants in §5, or objects pop in and out inside the margins. Objects do keep 9-bit OAM X, so they draw in the margins once alive.
* **Vertical levels are exactly 256 px wide** (right half of the maps empty): pillarbox them.
* Single-page areas (`$22` = 0): boss rooms, bonus rooms behind pipes, the King's room (tileset 2) and the toad house (tileset 7).
  * Their level data ends at the page. The port carries on their edge columns.
  * The toad house's black around its frame is BG1 tiles. The backdrop is purple through colour math, so the port masks the margins black.
  * The spade game (tileset `$0F`, NMI `$C0`) is a framed picture on black. Its white backdrop shows in blank margins, so the port carries on its edge pixels.
* **Level ends:** centred on the game's screen, the wide view shows past a level's start and end. The auto-scroll bounds (`$27:86BC`) and the right-side death (`$23:C324`) then test the player against the view's edges clamped to the level (`0` .. `($22 + 1) * 256`), so he stays inside it.
* **Screens that are 256-px compositions** (keep 4:3 or pillarbox):
  * title (`$0100=$20`), world map (BG2 frame, one map screen in VRAM), toad house (BG1 frame art), spade game (`$0101=$60` slider splits), king's room and message boxes, 2P battle arena (`$070A=$12`, no HUD)
  * pause menu and game-over panel: centred boxes; the pause box uses HDMA ch3 window (`$0293=$08`, TMW=`$15`)
  * the map iris transition (window)
* **Airship** uses BG3 band scroll IRQs at 145/158/165/172.
* **BG2** background is a 512-px static repeating pattern and **BG3** repeats every 256 px. Both wrap cleanly in wider views, but BG2 has parallax rules per background type (`$02BF`).
  * vramlog while scrolling found no BG2/BG3 map writes in 1-1, the W5 fortress or the airship.
  * Water levels rewrite BG3 map `$5A00-$5A7F` (4 rows) every frame for the animated surface band. It is still 256-px periodic.
* **Tiles above 255** come from the high page `$7E4000`. A self-renderer must honour it.
* A self-renderer should read tile memory as it was at vblank, not live. Tile changes reach VRAM a frame after they reach RAM (§4), so live reads change the margins a frame before the 4:3 picture. The port copies `$7E2000`/`$7E4000` (`$1950` bytes each) in its frame-start hook.
* The **hidden world select** (title state 7) is still in the ROM.
* Title layers:
  * BG1 holds the logo, the green hills cut by both screen edges, and the 1P/2P/Battle menu text.
  * BG2 holds the sky, the mountains and the checkered floor, all repeating across the screen.
  * The hills reach about 45 px in from the edges, from line 97 (right) and 129 (left) down.
  * The logo's letters come within 36 px of the right edge down to line 100.
  * The widescreen margins mirror BG1, line by line, as far as the shape touching the edge reaches
    (allowing up to 16 px of sky first, for the hill tops). So the hills and the small swirl at
    the left edge are mirrored, while the logo and menu text, clear of the edges, are not.
  * A fixed 48 px mirror cut the hills off with a straight edge.
  * BG2 carries on its edge pixel: mirroring left lines of the mountains' foot in the margins.

States used: `s_after_start` (title menu), `s_map_on11` (W1 map on 1-1), `s_11`/`s_11b` (1-1), `s_13`, `s_fly2` (1-1 cam ≈ `$402`), `s_vert` (7-1 pipe maze), `s_airship`, `s_toad`, `s_spade`, `s_castle`, `s_w8map`, `w8f`, `w5f`, `w7_1`, `s_pause`, `s_gameover` (all in `re_B/`).

## 8. Progress signals (used by the achievements, src/game/achievements.cpp)

| event | how it shows in RAM | tested |
|---|---|---|
| airship beaten | wand caught → king's room (`$070A=2`, world unchanged) → `$0727` becomes w+1 **in the king's room** → the map comes up with the new world. On the map the world goes w → w+1 with no map frame between. | W1: Larry stomped, wand, king, W2 map |
| Warp Zone | map with `$0727==8`; the world after it can be any | poke |
| Bowser defeated | Bowser is object `$18` (castle area A objects `$27FB7D`, columns `$CC`/`$EC`, row `$0B`). His ground pound breaks the floor and he falls below the level; **his slot stays `$18` with Y ≈ `$401`** until the ending. The time is then tallied into the score (with an infinite-time cheat the tally never ends and nothing more happens), and the princess's door at the top of the stairs (`$CE0`) opens. | fight played with a closed-loop script |
| ending | Up at the open door: `$0100` becomes `$20` (like the title) while `$070A` stays 2 (the title has 0) | yes |
| Toad House chest | item added to `$1D80+` while `$070A==7` (after the message has finished) | W1 Toad House, leaf |
| 100th coin | `$1DA2` 99 → 0 and lives `$0736` +1 | 1-1 |

To reach Bowser without the maze: from `bowser_start` (area A) write Mario `$43/$5E = $0BE0`, `$55/$70 = $0150`,
camera `$0210/$0211 = $0B60` and `$12 = $0B`, then walk right. The camera crosses column `$CC` and he
spawns. The background is stale (nothing re-streams it), but collision and objects are correct.

## 9. World 1 fortress, raster-split bands and camera jumps

* Object IDs seen here: `$52` = treasure chest (the Warp Whistle chest), `$4B` = Boom-Boom, `$9E` rotodisc
  pivot, `$3F` Dry Bones. Main area objects `$27F8DB` (Boom-Boom in the room behind the wall at columns
  `$90-$9E`); the door at column `$6D` (tiles `$00` over `$43`) leads to the alternate area, objects
  `$27F901`: the moving-floor room (NMI mode `$A0`) with the chest at column `$4A`, row `$18`, behind its wall.
* NMI mode `$A0` (also 3-2 and the tanks): the NMI writes BG1HOFS from `$0210` for the next frame's top part;
  an IRQ at line 160 (`$20:F39E`) writes BG1HOFS again with the camera this frame's logic has just moved,
  and its own BG1VOFS (`$EF` in the fortress room), so the band below runs one frame ahead of the rest.
* That door changes area without a dark frame (the level stays in a gameplay NMI mode): anything keyed on
  "the screen went dark" misses it; the camera jumps from about `$660` to 0 in one frame.

## 10. Level controllers, auto-scroll edges and spawn sides (QA 2026-10-19)

* The spawn routine (`$29:9AE0`) handles level objects `$B4-$BB` and `$D1-$D6` itself, at the moment
  their column is checked, instead of giving them a slot: `$B4-$BB` set `$0566` to id-`$B3` and return
  without marking themselves spawned (so the mode is set again on every frame the check is on that
  column), `$D3` starts auto-scrolling (`JSR $9C43`, its row byte a script), `$D4` writes `$7E3964/5`,
  `$D5` increments `$05FD`, `$D6` writes `$7E3963`, `$D1/$D2` `JSR $9C8B`. `$BC-$D0` and `$D7+` are
  cannon/fire generators (`$9D2D`). `$41` uses slot 6. Matched objects reach `$29:9B37` (X = index,
  `$1B3F+Y` its id); `$29:9B0E` continues the loop.
* Level object list in RAM: id `$1B41+3i`, position `$1B42+3i` (page<<4 | column), row `$1B43+3i`;
  first index per page `$1B00+page`; spawned flags `$1B10+i` (bit 7).
* Auto-scroll: the update at `$27:8500` pauses while `$0554 | $0551 | $9C` is set. `$9C` = `$BF | $0571 |
  $0554 | $0555 | $0551` (`$23:C030`); the edge test at `$23:C324` sets `$BF` = 1 (death) for screen X
  `$79 >= $F8`. The far-edge hold (`$27:86BC`) keeps the player's speed `$8B` at the scroll's `$1A0E`.
* Scroll direction `$25` (0 right, 1 left) picks the spawn check's side (`LDY $25` at `$29:9AE0`); it is
  1 at a level start. Some objects set up from it (4-1's chasing fish `$3B`).
* The cloud treasure areas (QA states `ch13/15/37/64/78`, named after levels 1-3, 1-5, 3-7, 6-4, 7-8):
  `$D3` auto-scroll, three pages; the chest `$52` at the end. At 21:9 holding right reaches the level end
  long before the camera does.

## 11. Goal walk and world map progress (2026-10-22)

* **Goal card** (object `$41`, special slots 5-7). Mario takes it from above/falling onto it. Its walk-off state calls `$28:CD4F`, which hides the player's 12 sprites (from `$C4`) whose X wrapped past the screen (X < `$40` → Y `$F0`, store at `$CD5E`). `$28:CAB9` holds the walk (`$0559` = `$FF`; the player code `$20:E911` walks at `$8B` = `$14` while it counts) and at `$28:CAE5` stops him (`$0556` = `$FF`, then a countdown at `$20:E85B`) once `$79` = `$20` (32 px past the edge). `$28:CB2F` goes on to the tally on the same test. The port moves both tests to 32 px past the wide view's edge and places the sprites at their true X.
* **Clear bits**: `$1D00` + column (`xhi * 16 + x / 16`; Luigi + `$40`), one bit per row: Y `$20`-`$80` → `$80`-`$02` (`$21:E8A4` / `$21:E8AB`), anything lower → `$01` (the reload then searches down from row `$80`). A fresh clear: `$29:AFC9` replaces the panel by `$21:E5C2,X` (by the tile's top bits and the player; `$67`/`$6A` → `$60`, `$EB` → `$E3`), and `$29:BC69` sets the bit, for both players when it became `$60`/`$E3` (fortresses).
* **Map reload** (`$29:BE62` copies the layout from ROM, then `$29:BEB6` applies the bits): numbered panels become the M/L tile; tiles in `$21:D790` become `$21:D798`: `$51`/`$52`/`$56` → `$45`, `$54` → `$46` (locks, rocks), `$67` → `$60`, `$EB` → `$E3`, `$E4` → `$DA`, `$9D` → `$B3`. A lock opens by its own position's bit (World 1's at Y `$50`, column 4: `$1D04` bit `$10`).
* **Layout**: screen s at `$7E2000 + s * $1B0`; a cell is at `+$100 + (Y - $10) + X / 16`. Numbered panel n is tile `$02 + n`.
* **Movement** (`$23:8E15`, data bank `$21`): the pad direction (`$21:8DB4`: R L D U) picks the cell next to Mario (`$21:8DFC` Y, `$21:8E04` X offsets) and a 9-tile walkable list (`$21:8DC4` across, `$21:8DD6` up and down; `$45`/`$46` are the plain paths). Mario then moves two cells. World 4 on lead through map pipes, which the lists don't show.
* **Level select** (`$072B`): the menu cursor until Start, then the number of players. A level's return to the map copies each player's map position back from `$7E3975+` (`$20:817B`) that many times.


## Saves and projectiles (2026-11-19)

* **Save** `$00:922D` (from the map's pause menu, `$29:EE07`; Start confirms, cursor `$028E`): only
  when the world `$0727` is at least the file's; writes the world, then `$7E1D00-$1D7F` (all the
  clear bits) and `$38` bytes from `$1EED` into the file (`$70:0010 + [$70:0004]`, `$4C` on).
* **Load** `$20:81B5`: lives, items; if the file's world is the one starting (`$7FFB00` =
  `$7FFF02`), the clear bits back to `$1D00` (`$20:8204`), then `AND $8000,Y` (`$20:8225`, data
  bank `$21`: a 64-byte mask per world, `$21:8000 + world * $40`) keeps only some locks and
  fortresses, copied to Luigi's `$1D40` too.
* **Projectiles**: two slots, `$1CE1,X` (1 fireball, 2 hammer, 3+ puff); `$27:B42C` runs slot 1
  (JSR `$B436`) then falls into slot 0, back at `$27:B2B7`. Per slot: `$1CE1/3/5/7/9/B/D,X`,
  `$0247,X`, `$05FA,X`. `$9B` the slot being run; `$C6,X` (16-bit) its sprites' offset, found
  free each time by `$27:D728`. `$0783,X` is the random number generator, not the slot's.
* **Suit on the map**: `$0747`/`$0748` (Mario / Luigi) is what the next level starts with; the map
  draws `$0429` (set from it when an item is used). The save doesn't keep either: a file starts small.
* SRAM `$70:04A0-$1FDF` is never read or written by any of the games or the menu (watched over boot,
  the menu, all four games' play and saves). Save Level Progress keeps its power-up records at
  `$70:0600` on.
