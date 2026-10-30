# Deadlock Build Helper

An external overlay: while you hold **TAB** in Deadlock it grabs the screen and, with OpenCV
template matching on the scoreboard, works out for each enemy:

- which **hero** they play (portrait recognition),
- which **items** they own, and from that their **build** (gun / spirit / tank / hybrid),
- **tips** for playing against those items (e.g. "Unstoppable: bait it out before your CC"),
- items that do **badly** against their hero.

It then ranks **what to buy** against all of that together, in a transparent, click-through
Dear ImGui panel, in buying order: each item with its picture, shop tab, tier and price, when to buy it,
and what it builds on. Enemy purchases since your last look are listed, and every few minutes a small
note reminds you of the next purchase. Press **F5** to keep the panel up while you shop. Counters come from `assets/counters.json`
(see "Counter data").

## What it does and does not touch

| Does | Does not |
| --- | --- |
| Reads the composited desktop image (DXGI Desktop Duplication), i.e. the pixels on your monitor | `ReadProcessMemory` / `WriteProcessMemory` |
| Polls key state with `GetAsyncKeyState` | Install keyboard/mouse hooks (`SetWindowsHookEx`) |
| Reads the foreground window's **title** to know the game is focused | Open a handle to the game process (`OpenProcess`) |
| Draws its own topmost window | Inject DLLs, hook D3D/Present, or send input to the game |

Being external keeps it out of the game's memory, but it is not an official endorsement:
check Valve's current rules for Deadlock before you use any third-party overlay.

## Hotkeys

All of them only react while the Deadlock window is focused, and can be changed under
`"hotkeys"` in `config.json`.

| Key (default) | Action |
| --- | --- |
| **TAB** | Show the panel. A short press is enough: it stays up for `panel_hold_ms` after release |
| **F10** | Settings menu: pick your nickname, move the boxes and the panel, thresholds, hotkeys |
| **F5** | Pin the panel: the last result stays on screen after TAB is released, for instance over the shop |
| **TAB + F6** | Auto-tune the icon size to your screen; saved to `assets/calibration.json` |
| **F7** | Toggle calibration mode: enemy rows (yellow) and live detections (green) drawn over the game |
| **F8** | Reload `config.json`, `counters.json` and the templates without restarting |
| **F9** | Scan and save `debug_capture.png` + `debug_roi.png` next to the exe |
| **Ctrl+End** | Quit (works anywhere) |

## Project layout

```
deadlock-build-helper/
├─ CMakeLists.txt          build script (option DBH_HIDE_CONSOLE)
├─ CMakePresets.json       "cmake --preset default" wiring for vcpkg
├─ vcpkg.json              dependencies: opencv4[png,thread], imgui[dx11,win32], nlohmann-json
├─ assets/                 copied next to the .exe on every build
│  ├─ config.json          hotkeys, enemy rows, thresholds, panel position
│  ├─ counters.json        items / builds / heroes -> counters (generated, see "Counter data")
│  ├─ templates/           one PNG per item icon (not in git: tools/fetch_icons.py); ITEM_IDS.txt lists the names
│  ├─ icons/               active items' shop art, for the panel's pictures only (not in git: tools/fetch_icons.py)
│  └─ heroes/              one PNG per hero (not in git: tools/fetch_heroes.py); HERO_IDS.txt lists the names
├─ tests/
│  ├─ logic_tests.cpp      config, counters, ranking, per-enemy analysis
│  ├─ vision_tests.cpp     synthetic scoreboards through the real VisionEngine
│  └─ screenshot_check.cpp the whole recognition on a saved screenshot (a tool, not a test)
├─ tools/
│  ├─ build_counters.py    regenerates counters.json from the live game data and match statistics
│  ├─ fetch_icons.py       downloads the passive item icons as ready-made templates
│  └─ fetch_heroes.py      downloads the hero card art as ready-made portrait templates
└─ src/
   ├─ main.cpp             main loop: hotkeys, scan timing, calibration toggle
   ├─ Types.h              Detection / EnemyPlayer / Suggestion / AnalysisResult
   ├─ Config.{h,cpp}       config.json loader, key names
   ├─ ScreenCapture.{h,cpp}   DXGI Desktop Duplication -> cv::Mat (BGR)
   ├─ VisionEngine.{h,cpp}    item icons: fast two-pass scan per column, thorough search, size tuning
   ├─ HeroRecognizer.{h,cpp}  which hero a portrait shows, from the card art at many scales
   ├─ CounterDatabase.{h,cpp} counters.json + suggestion ranking (items, heroes, builds)
   ├─ PlayerAnalysis.{h,cpp}  detections -> per-enemy hero, items, build, tips
   ├─ AnalysisWorker.{h,cpp}  background thread: capture -> vision -> per-row grouping, reloads
   ├─ PanelView.{h,cpp}    the panel's layout and look; needs only Dear ImGui
   ├─ SettingsMenu.{h,cpp} the F10 menu; needs only Dear ImGui
   └─ Overlay.{h,cpp}      transparent click-through window, item pictures, calibration drawing
```

Flow: the main thread renders the overlay and polls the hotkeys. Shortly after TAB goes down it
asks `AnalysisWorker` for a scan. The worker grabs a frame, matches the templates against each enemy
column, recognises the portraits, ranks counters, and publishes the result. The worker owns the templates and the counter table,
so a reload (F8) happens on its thread between scans and needs no locking around the vision code.

The overlay window is excluded from capture (`WDA_EXCLUDEFROMCAPTURE`), so it never detects its
own panel or calibration boxes.

**Keeping the game's frame rate.** Between scans the helper costs almost nothing:

- The window is hidden whenever nothing is drawn. When shown it is only as big as the panel, and it
  redraws only when the content changes, so DWM has little to blend.
- The screen capture is closed 3 seconds after the last scan: an open Desktop Duplication keeps
  Windows compositing for it.
- Scans happen only on a TAB press (the scoreboard, and so the items, are not on screen otherwise), at
  most two per press, on a below-normal-priority thread, limited to `scan_threads` CPU
  threads (default 2). It first looks for each icon in grey at half size, and checks in colour only
  what looks promising.

## Building on Windows (step by step)

You need Windows 10 version 2004 or newer, or Windows 11.

1. **Install Visual Studio** (Community is free, 2022 or newer). In the installer select the
   **Desktop development with C++** workload. It includes the MSVC compiler, the Windows SDK
   (DirectX 11 headers), CMake, Ninja and vcpkg.
2. **Open the project:** in Visual Studio use **File → Open → Folder…** on the project folder.
   It reads `CMakePresets.json` and picks the **Release (x64, vcpkg)** preset. The first
   configure installs OpenCV, ImGui and nlohmann-json through vcpkg, which takes 10–30 minutes
   (OpenCV compiles from source); later runs reuse the cache. Watch the Output window.
3. **Build and run:** select `DeadlockBuildHelper.exe` as the startup item and press **F5**
   (or **Build → Build All**). The exe lands in `build\` next to an `assets\` folder, and vcpkg
   copies the OpenCV DLLs there too. A console window shows logs; quit with **Ctrl+End**.

From the command line instead: open **Developer PowerShell for VS** (it has the compiler,
Ninja and Visual Studio's vcpkg on the path), go to the project folder and run
```powershell
cmake --preset default
cmake --build --preset release
.\build\DeadlockBuildHelper.exe
```

The `debug` preset builds an unoptimised version into `build-debug\` - use it only for
debugging, template matching is several times slower there.

`vcpkg.json` sets a `builtin-baseline` (which Visual Studio's built-in vcpkg requires) and
`version>=` minimums, so you always get the tested OpenCV 4.14.0, ImGui 1.93.0 and nlohmann-json
3.12.0, even if the baseline is changed. If your Visual Studio's
vcpkg is too old for that baseline (errors about unknown fields or versions), update Visual
Studio, or install a standalone vcpkg and point `VCPKG_ROOT` at it:
```powershell
git clone https://github.com/microsoft/vcpkg C:\dev\vcpkg
C:\dev\vcpkg\bootstrap-vcpkg.bat -disableMetrics
setx VCPKG_ROOT C:\dev\vcpkg
```
then restart Visual Studio / the terminal and delete the `build` folder.

Once calibrated, you can drop the console window:
```powershell
cmake --preset default -DDBH_HIDE_CONSOLE=ON
cmake --build --preset release
```
Logs then go to `build_helper.log` next to the exe.

## First-time calibration

The defaults in `config.json` were measured on a real 1080p TAB scoreboard (see "Testing"),
so at 16:9 the boxes should already sit on the scoreboard. Every player is a **column** at the
top of the screen: portrait card, stats, then a strip of item icons two wide. Your team is on
the left, the enemy team on the right.

1. In Deadlock set **Display Mode: Borderless Windowed**. Exclusive fullscreen hides topmost
   windows, so the overlay would not show. Turn **Windows HDR off** for that monitor (HDR
   desktops are captured as 16-bit float and are rejected).
2. **Item templates - download them** (Python 3.9+, no extra packages):
   ```powershell
   python tools\fetch_icons.py
   ```
   This saves the shop icons of all **123 passive items** into `assets\templates\`. The
   scoreboard shows exactly that artwork; the tier badge it adds in the top-right corner is
   ignored (`item_corner_mask`). The 50 active items' art goes to `assets\icons\`, for the panel's
   pictures. Rebuild once so they are copied next to the exe.
3. **Active items - crop them once.** The scoreboard draws the 50 active items differently
   (round, dark, with a localized "ACTIVE"/"AKTYWNY" label), so their shop art does not match.
   When an enemy owns one, hold TAB, press **F9**, open `debug_roi.png` (next to the exe) in
   Paint/GIMP, crop the icon tightly and save it as `build\assets\templates\<id>@active.png`.
   `fetch_icons.py` and `assets\templates\ITEM_IDS.txt` list all ids, e.g. Unstoppable is
   `unstoppable@active.png`, Healing Rite `health_stimpak@active.png`. Press **F8** to load it.
4. **Hero portraits - download them:**
   ```powershell
   python tools\fetch_heroes.py
   ```
   This saves the card art of all 44 heroes into `assets\heroes\` as `abrams.png`,
   `lady_geist.png`, ... Rebuild once so they are copied next to the exe. From then on every enemy
   is listed with counters against their hero, even before they own any item.
5. Press **F7** in a match: yellow boxes show where the helper reads each enemy's items, cyan
   boxes the portraits, green / magenta boxes what it found and with which score. If a box is
   off (other aspect ratio, different HUD), edit `enemy_rows` / `enemy_portraits` in
   `build\assets\config.json`: `first` is the enemy closest to the centre, `step_x` the
   distance between columns, all as fractions of the screen. Save, press **F8**, hold TAB.
6. **Icon size:** hold TAB and press **F6** while enemies own a few items. The helper tries
   icon scales from 0.80 to 1.25, keeps the one that finds the most icons, and saves it to `assets\calibration.json` (survives rebuilds, overrides
   `icon_scale`). Needed when your HUD scale or resolution makes the icons a different size
   than 27 px per 1080 px. Takes 5-15 seconds.
7. **Thresholds:** wrong items -> raise `match_threshold`; real ones missed -> lower it a little
   (and press F6 first: a size mismatch looks the same). Heroes need `hero_threshold` and a lead of
   `hero_margin` over the next-best hero; **F9** logs both numbers for every portrait.
8. When you are happy, **copy** `config.json`, `calibration.json` and your crops from
   `build\assets` back into the project's `assets` folder: every build overwrites the copy in
   `build\` (except `calibration.json`, which only exists there).

**Check a screenshot without the game.** Build with `-DDBH_BUILD_TESTS=ON` and run
```powershell
.\build\screenshot_check.exe C:\path\to\shot.png build\assets --allies --out checked.png
```
It runs the same recognition as the app on a saved screenshot, prints every enemy's (and
with `--allies` your team's) portrait scores, items, build and the suggestions, and draws the boxes
into `checked.png`.

## Settings menu and finding you

Press **F10** in a match. The menu takes the mouse and keyboard until you close it (**F10**, **Esc** or
**Close**); the game keeps running behind it, so don't open it mid-fight. It shows the scoreboard from
your last scan, with the boxes the helper reads drawn on it. Changes apply when you release a slider and
are saved to `assets\settings.json` next to the exe: only what differs from `config.json`, and they
survive rebuilds. **Reset to defaults** deletes that file.

**You.** Hold TAB once, open the menu and click your nickname among the green boxes. The helper saves a
picture of it (`assets\my_name.png`) and from then on finds you by that picture, in whichever column and
on whichever hero. The panel then shows your hero and items, warns about items of yours that do badly
against these enemies, leaves what you own out of the suggestions, and ranks answers to a threat lower
when you already own one.
Pick it again if you change your nickname or resolution. It does not read text, so nothing is typed: a
typed nickname would need OCR. On two real frames the nickname scored 0.998 or more, and other text at
most 0.46.

**Teaching icons.** Active items look different on the scoreboard and cannot be downloaded (their API art
scored 0.45 on a real icon, below other items). Under "Teach an icon", click one on the enlarged strips and
pick its name; it is saved as `assets\templates\<id>@active.png` and recognised from then on. Copy these
into the project's `assets\templates` if you want them to survive deleting `build`.

## When to buy

The panel sorts its suggestions into **NOW / NEXT / LATER** by what you have spent so far: your own items'
cost once your nickname is set (F10), otherwise what the average enemy owns. Under 3000 souls tiers 1-2
are "now", under 15000 tier 3, above that tier 4. That is a rule of thumb. "Usually min N" is when players
actually buy the item (public match data), and "Cheaper after X" / "Upgrades your X" name the item it
builds on; the price shown is what is left to pay. The helper cannot read your current souls, so it does
not know what you can afford this second.

A **reminder** names the next "now" item every `reminder_minutes` (0 = off), for 8 seconds, as long as you
have scanned recently. Set it in the menu.

## Counter data

`assets/counters.json` is generated by `tools/build_counters.py` from the public
[Deadlock API](https://api.deadlock-api.com/docs). It has three layers, all ranked together:

| Layer | Source | Example |
| --- | --- | --- |
| **items** (173) | Game data (names, slots, tiers, descriptions) + rules in the script | Healing Rite → Toxic Bullets, Healbane, Decay… |
| **archetypes** (3) | Rules in the script | Spirit build → Spirit Resilience, Spirit Shielding… |
| **heroes** (44) | Match statistics, last 30 days | Haze → Metal Skin (+2.0 pp win rate, 729k games) |

**Item rules.** Enemy items are grouped by what they do: healing, gun burst, spirit burst,
ability spam, stun, silence, disarm, slow, escape mobility, melee, resist shred, bullet
build-ups, DoTs, stealth, defensive actives, and anti-heal on you. Every counter has a priority
(3 = first pick, 2 = good, 1 = situational) and a one-line reason. Overrides fix cases where a
generic answer fails: no bullet resist against Armor Piercer or Infinite Rounds (they ignore it),
no spirit resist against Tankbuster, no Rebuttal against Runed Gauntlets (pierces parry), no
Dispel Magic against Cursed Relic (blocks item use). 45 items also carry a **tip** on how to play
against them (Unstoppable, Counterspell, Metal Skin, Cheat Death, Healing Rite, ...). 50 items
have nothing to counter on their own (plain stat items such as Extra Health, or the counter
items themselves); they still feed the build detection.

**Hero counters from match data.** For each hero the script asks the API for item win rates in
games where that hero was on the enemy team, and compares each item with its own overall win
rate:

    lift = (item win rate vs hero − item's overall win rate) − average shift of all items vs that hero

Comparing an item with itself removes "late-game items always look good"; subtracting the
average shift removes "this hero is weak, so everyone facing it wins more". Only lifts above
two standard errors and at least 0.3 percentage points are kept, from items with 3000+ games.
The three worst items per hero become the panel's "avoid" line.

This is a correlation, not an experiment: players who buy an item against a hero also differ
in skill and situation. Read +2 pp as "clearly works in practice", not as a guarantee.

**Ranking.** Each suggestion's score is the sum over everything it answers of
`count × weight × priority`. Item weights are 1–3 (hard CC is 3), heroes and builds weigh 2.

**After a patch**, regenerate it (Python 3.9+, no extra packages, about 2 minutes):
```powershell
python tools\build_counters.py
```
If the patch renamed or removed an item the rules use, the script lists those ids and stops.
`--no-stats` skips the match statistics; `--stats-dir stats` saves the API responses so a run can
be repeated offline with `--offline`. Edit the rules in the script, not the JSON.

## Testing

```powershell
cmake --preset default -DDBH_BUILD_TESTS=ON
cmake --build --preset release
ctest --test-dir build --output-on-failure
```

- `logic_tests` (174 checks): config parsing, the measured column layout, value clamping, key
  names, `calibration.json`, the counter database (items, heroes, builds, tips, overrides such
  as "no spirit resist vs Tankbuster"), ranking, and the per-enemy hero / item / build analysis.
- `vision_tests` (275 checks): Unicode-safe image IO, hero template loading, then synthetic
  scoreboards in the real layout (27 px icons two per row in each enemy column, hero art drawn at
  0.25-0.29x from the top of the screen) with templates for all 173 items and 44 heroes, the app's
  masks and thresholds, blur, brightness shift, noise, unknown "distractor" icons, an icon between
  two columns, a hero without a template and an empty portrait box. At 1080p and 1440p every item lands in the right enemy, every hero is recognised,
  nothing unknown is reported and builds are classified correctly. Icons drawn 0.88x, 0.95x and
  1.12x too small/large: plain search finds 0, 0 and 13 of 19 items, after F6 tuning all 19. The fast
  scan must find exactly what the thorough search finds.

Both also pass under AddressSanitizer + UndefinedBehaviorSanitizer, and the vision tests
under ThreadSanitizer (the template search runs in parallel).

**On a real 1080p screenshot** of the TAB scoreboard (sandbox match, webp-compressed):

| Check | Result |
| --- | --- |
| Downloaded API icons only (123 passive), `screenshot_check --allies` | all **8 of 8** passive items found, scores 0.950-0.977, build "Spirit build" correct |
| Same, enemy columns (enemy owned no items) and the rest of the screen | **0** false hits |
| Without the corner mask | only 3 of 8 found: the mask is required for API icons |
| Two *different* real icons | at most 0.561 alike (threshold 0.90) |
| `downscale` 0.75 instead of 1.0 | weakest real match drops from 1.000 to 0.891 with own crops: keep 1.0 |
| Active item (Cold Front) vs API art | not recognisable (0.58): crop actives yourself |

**Heroes**, on two real 1080p frames (the second one a live capture from the app), with the
downloaded card art only:

| Check | Result |
| --- | --- |
| Occupied portraits (Abrams twice, Infernus twice, Venator, Lash) | **6 of 6** right, scores 0.875-0.952, at least 0.41 ahead of the next hero |
| 18 empty slots over the game world | **0** false hits: best 0.706, lead at most 0.17 |
| Same frames resampled to 720p and 1440p | still 6 of 6 and 0 false hits, scores 0.852-0.949 |
| 60 random game-world patches | at most 0.776, lead at most 0.282 (hence 0.80 and 0.25) |

Speed (Apple M-series, one thread, whole scan with 123 templates and six portraits): ~350 ms with the
fast scan against ~3450 ms with the thorough search (`screenshot_check --thorough`), and the same items
found. F6 tuning takes under a second on all threads.

## Troubleshooting

| Symptom | Fix |
| --- | --- |
| Few or no items detected, everything else looks right | Icon size mismatch: hold TAB and press F6. |
| Game stutters when you press TAB | Check that you run the Release build (`build\`, not `build-debug\`), and try `"scan_threads": 1`. The panel's status line shows how long a scan took. |
| Lower FPS while the panel is up | Windows composites the game with any window over it. Release TAB or unpin (F5); FPS should return as soon as the panel is gone. |
| Panel says `no hero portraits` / `no item icons` | Run `tools\fetch_heroes.py` / `tools\fetch_icons.py`, then rebuild. |
| An enemy shows as `(hero unknown)` | Hold TAB, press F9 and read the console: the score and the next hero's for each portrait. A skin may need its own crop (see `assets\heroes\README.txt`). |
| `monitor_index ... does not exist` | Set `monitor_index` (0 = first monitor found, across all GPUs). It needs a restart, not just F8. |
| `unsupported desktop format` | Windows HDR is on for that monitor. |
| Overlay never appears | Game is in exclusive fullscreen, or `game_window_title` does not match the window title exactly. |
| `could not exclude overlay from capture` | Windows older than 2004: place `panel` outside the enemy rows, and expect calibration boxes to disturb live scans. |
| `reload failed, kept previous config` | `config.json` has a syntax error; the message says where. |
| Items show up under the wrong enemy | Column boxes are offset; fix `first`/`step_x` in calibration mode. |
| `vcpkg.cmake` not found during configure | `VCPKG_ROOT` is not set: use Developer PowerShell for VS (or Visual Studio itself), or `setx` it for a standalone vcpkg. |
| `CMAKE_CXX_COMPILER not set` | Follows a failed vcpkg step - fix the vcpkg error above it. In a plain terminal it also means you are not in Developer PowerShell. |
| vcpkg says a `builtin-baseline` is required | You have an old copy of `vcpkg.json`; the current one sets it. Delete the `build` folder and configure again. |
| `opencv4@4.8.0#... does not have required feature thread` | `vcpkg.json` has an old `builtin-baseline` (e.g. added by `vcpkg x-update-baseline`). Use the shipped `vcpkg.json`: its `version>=` entries force OpenCV 4.14, ImGui 1.93, nlohmann-json 3.12 whatever the baseline. Don't run `x-update-baseline` with Visual Studio's vcpkg, it writes its own (2023) baseline. |
| vcpkg errors about unknown fields / versions | Visual Studio's vcpkg is older than the baseline: update Visual Studio or use a standalone vcpkg (see Building). |

## Ideas for next steps

- Mark the suggested items right on the shop screen (needs a screenshot of the shop to measure it).
- Detect your own items as well and hide suggestions you already own.
- Hero-vs-hero data (`/v1/analytics/hero-counter-stats`) for draft advice before the match.
