# AGENT.md — MatrixGame

Working rules and project map for AI agents (and humans) who modify this repository.
Derived from the actual state of the `refactoring` branch (HEAD `549647e`), not from the README.

## 1. What this is

Game engine for the strategic mode of **Space Rangers 2: Planetary Battles** (the MatrixGame engine).
C++11, Win32, DirectX 9, **x86 only**. Licensed under GPLv2 or later (see `LICENSE`).

- Repository: `https://github.com/AlLitv/MatrixGame`
- Branch: `refactoring` — the only one; `origin/HEAD` points to it.
- Upstream fork: `https://github.com/twoweeks/MatrixGame`; original sources: `snk-games.net/matrixgame.tar.gz`

| Parameter | Value |
|---|---|
| Languages | C++ (252 files), C (110), MASM (`Bitmap/asharpen.asm`) |
| Platform | Windows, x86 (`/MACHINE:X86`), 32-bit |
| Graphics | DirectX 9: `d3d9`, `d3dx9`, `DxErr` |
| Build | CMake ≥ 3.13 + MSVC (officially VS2010/2012) |
| Default artifact | **DLL** (`MATRIXGAME_BUILD_DLL=TRUE`), EXE otherwise |
| Tests | none at all (no directories, no framework) |
| Encoding | UTF-8, LF line endings |
| Formatting | clang-format, `.clang-format` (Chromium base, indent 4, width 120) |

## 2. Build

```bat
cmake -S . -B build -A Win32
cmake --build build --config Release
cmake --install build --config Release
```

- `-A Win32` is mandatory: the project is 32-bit only.
- Install output: `bin/` (Release), `bin/Debug/` (Debug, plus `.pdb`). `build/` and `bin/` are in `.gitignore`.
- The DirectX SDK (June 2010) is located by `cmake/FindDIRECTX9.cmake` via `DXSDK_DIR`, `DIRECTX_ROOT` or the standard install paths. Without it configuration fails with `Could not find DirectX SDK`.
- MinGW: CMake has a `-std=c++11 -fpermissive` branch (commit "Slightly change CMakeLists for mingw"), but `asharpen.asm`, `.rc`/`.ico` and `d3dx9` make MSVC the only realistic path. Do not assume a MinGW build works without verifying it.

**Building on a modern toolchain (VS2022, x86).** The stock 2012-era build needs four fixes for the current MSVC toolset; all are already applied in this tree:

- `WINDOWS_IGNORE_PACKING_MISMATCH` — the project compiles with `/Zp1`, and the current Windows SDK raises a `static_assert` in `winnt.h` on non-default packing. Pass it globally:
  ```bat
  cmake -S . -B build -A Win32 ^
    "-DCMAKE_CXX_FLAGS=-DWINDOWS_IGNORE_PACKING_MISMATCH" ^
    "-DCMAKE_C_FLAGS=-DWINDOWS_IGNORE_PACKING_MISMATCH"
  ```
- `/Od` for `src/MatrixLogic.cpp` — MSVC 19.34/19.37 dies with an internal compiler error (C1001) in the codegen pass; `/Ob0` is not enough (`MatrixGame/CMakeLists.txt`).
- `/safeseh` for `MatrixLib/Bitmap/asharpen.asm` — otherwise linking fails with `LNK2026`/`LNK1281` (`MatrixLib/CMakeLists.txt`).
- `legacy_stdio_definitions` — `DxErr.lib` from the June 2010 SDK references `__vsnwprintf`, which the UCRT no longer exports (`CMakeLists.txt`).

The DirectX SDK installer may report **`S1023` / "Setup failed"**: that concerns only the DirectX runtime component. Headers and libs are installed anyway — verify `Include/d3dx9.h` and `Lib/x86/d3dx9.lib` exist.

**The standalone EXE build does not run.** `MATRIXGAME_BUILD_DLL=FALSE` compiles and links, but the process crashes inside `L3GInitAsEXE` (window/D3D device init) before the map is loaded: the SR2 HD config carries no video parameters, because in DLL mode they come from the host game. Use DLL mode for testing.

Options and defines:

| Name | Default | Meaning |
|---|---|---|
| `MATRIXGAME_BUILD_DLL` | `TRUE` | `MODULE` (DLL) instead of `add_executable` |
| `MATRIXGAME_CHEATS` | `TRUE` | defines `CHEATS_ON` |
| `MATRIXGAME_EXPORTS`, `USE_PNGLIB` | always | DLL exports, libpng instead of the built-in PNG code |
| `ASSERT_OFF` | Release | disables `ASSERT` |
| `EXE_VERSION` | when `MATRIXGAME_BUILD_DLL=FALSE` | standalone exe mode |

## 3. Layout

| Directory | Purpose |
|---|---|
| `MatrixLib/Base` | Infrastructure: `CHeap` (allocator), `CStr`/`CWStr`, `CFile` and packs, `CException`, `CBlockPar`, `CRC32`, `Tracer`, `Registry` |
| `MatrixLib/3G` | D3D9 renderer: `Texture`, `BigVB`/`BigIB`, `Cache`, `Form`, `ShadowProj`/`ShadowStencil`, `Math3D`, `CBillboard` |
| `MatrixLib/Bitmap` | `CBitmap`, `sharpen.cpp` + `asharpen.asm` (VirtualDub) |
| `MatrixLib/Window` | Win32 window wrapper |
| `MatrixLib/DebugMsg`, `FilePNG`, `FileJPG` | Logging, PNG (via libpng), JPG (via libjpeg) |
| `MatrixGame/src` | The engine itself: `MatrixGame.cpp` (entry point), `MatrixMap*`, `MatrixRobot`, `MatrixFlyer`, `MatrixLogic`, `MatrixCamera`, `MatrixRenderPipeline`, `MatrixSoundManager`, `MatrixWater`, `MatrixVisiCalc`, etc. |
| `MatrixGame/src/Logic` | Combat logic: AI groups, rules, road network, tactics, states |
| `MatrixGame/src/Interface` | UI: `CInterface`, form elements (`CIFace*`), menus, hints, constructor, history |
| `MatrixGame/src/Effects` | Visual effects: explosions, fire, dust, lightning, billboards |
| `ThirdParty` | Vendored `ZLib`, `LibPNG`, `LibJPEG` — **do not edit** |
| `Extras/MaxExp` | Separate 3ds Max exporter (its own CMake, requires the 3ds Max SDK) |
| `tools/` | Helper scripts: swap our `MatrixGame.dll` into an installed game, launch a battle with the determinism harness |
| `cmake/` | `FindDIRECTX9.cmake`, `ToolClangFormat.cmake` |

## 4. Entry point and lifecycle

- EXE: `_tWinMain` in `MatrixGame/src/MatrixGame.cpp:42` → `MatrixGameInit(...)` → `CFormMatrixGame` + `FormChange` → `L3GRun()` → `MatrixGameDeinit()`.
- Map passed on the command line (`args[1]`) instead of the menu: takes the branch with `g_MatrixMap->CalcVis()` and writes to `calcvis.log`.
- DLL mode: `MatrixGame/src/MatrixGameDll.cpp` — the bridge into the SR2 engine through `g_RobotInterface` (`SMGDRobotInterface`) and `g_RangersInterface` (`SMGDRangersInterface`); public entry point `Run` (`MatrixGameDll.cpp:244`).
- Static initialisation of the engine classes happens in `static_init()` in `MatrixGame.cpp`.

## 5. Mandatory coding rules

1. **Memory — only through the engine macros.** `HNew`/`HDelete`/`HAlloc`/`HAllocClear`/`HFree` from `MatrixLib/Base/CHeap.hpp`; bare `new`/`delete` are not allowed. The heap is passed explicitly (`g_MatrixHeap`); in Release builds the macros simplify to plain operators.
2. **Errors go through `CException`** and the `ERROR_S`/`ERROR_L` macros; tracing uses `DTRACE()`.
3. **`ASSERT` is compiled out in Release** (`ASSERT_OFF`). Checks that matter for correctness must not live only inside `ASSERT`.
4. **Naming:** classes `C*`, structs `S*`, members `m_*`, globals `g_*`, enum constants `UPPER_CASE`. The style is the engine's own, not modern C++.
5. **Includes must match the file name case exactly.** There is a dedicated commit, "Fix character case of includes": case matters on Linux/CI. Project headers are quoted.
6. **Formatting follows `.clang-format`.** The `clang-format` target from `cmake/ToolClangFormat.cmake` is **not wired into** `CMakeLists.txt` (the file is never included), so run it manually:
   ```bat
   clang-format -i -style=file <changed files>
   ```
   Never reformat unrelated files wholesale — keep diffs to the actual change.
7. **`/Zp1`** — structures are packed to 1 byte. The field order and types of serialised structures (maps, packs, file formats) must not change without a migration.
8. **`ThirdParty/`** is vendored code (zlib, libpng, libjpeg). Edit it only deliberately, together with a version bump — never "in passing".
9. Comments are mixed Russian/English, which is normal here. Files are UTF-8/LF; do not re-encode them.

## 6. Global state

Almost all code is tied to engine singletons: `g_MatrixHeap`, `g_MatrixMap`, `g_MatrixData`, `g_Render`, `g_Cache`, `g_Config`, `g_Flags`, `g_Wnd`, `g_D3D`/`g_D3DD`, `g_LoadProgress`. Initialisation is `MatrixGameInit`, teardown is `MatrixGameDeinit` (the order matters: map, config, icons, packs, heap).

## 7. Navigating the code

The repository carries a local **CodeGraph** index (`.codegraph/`, ignores itself in git, never committed).

- MCP tool: `codegraph_explore(query, projectPath="<path to MatrixGame>")` — returns verbatim symbol sources plus call paths and blast radius. Prefer it over blind grep-and-read loops.
- CLI:
  ```bat
  codegraph explore "CMatrixMap::CalcVis" -p <path>
  codegraph callers MatrixGameInit -p <path>
  codegraph impact CFormMatrixGame -p <path>
  codegraph sync -p <path>
  ```
- Index: 362 files, 9751 nodes, 34195 edges.
- Known parser warnings (not project defects, ignore them):
  - `MatrixLib/FilePNG/FilePNG.hpp` — `__cdecl` in a bodyless declaration trips tree-sitter; the function bodies are indexed from `FilePNG.cpp`.
  - `ThirdParty/ZLib/inffast.h` — the `OF((...))` macro; the file is a fragment included into `inflate.c`.

## 8. Determinism (netplay groundwork)

Six root causes of desync have been found; the first six subsections below are fixed, the seventh is open. Together they explain why a fixed seed and identical commands still gave a different battle at 7-17 s:

1. **Pointer comparisons used as tie-breakers.** `CollisionCallback` (`MatrixRobot.cpp`) decided which of two head-on robots yields via `DWORD(pCurrBot) < DWORD(data->robot)`. Heap addresses differ between runs, so the winner differed and the battle desynced after a few seconds. Map objects now carry a stable creation-order id (`CMatrixMapStatic::m_Id` / `GetId()`), and both comparisons use it.
2. **The logic AI and the renderer shared one RNG stream.** `RND`/`FRND`/`IRND` expand to `rand()`, and visual effects call them from `Draw`. Every extra draw (a visible repair beam, a dust puff, a debris look) shifts the sequence the AI consumes, so what is on screen desyncs what happens in the world. Every build now keeps two streams (`RndSplit.{hpp,cpp}`): `rand()` expands to `RndDispatch()`, which returns numbers from a logic xorshift outside the draw phase and from libc `rand()` inside it, and the whole frame including `BeforeDraw` runs in the visual phase. The harness plugs its instrumented implementation behind the same interface. This moved the first cross-fill divergence from 6100 ms to 9630 ms of game time and, more importantly, made visual noise unable to affect the simulation at all.
3. **Reading uninitialised memory.** A field read before being written returns heap garbage, which differs between processes. The fields that were provably read before being written are now initialised (list below), and `CHeap::Alloc` no longer touches fresh memory at all: the fill is a diagnostic switch, not a workaround.
4. **The graphic takt ran only for what was on screen.** `CMatrixMapStatic::SortEndGraphicTakt` walks `objects[]`, which is the per-frame list filtered by the camera frustum, so an object advanced only in the frames where it was visible — at 60 fps some ticks got two passes, some none. The graphic takt is not purely visual: it smooths the body up vector, and `RNeed(MR_Matrix)` rebuilds the forward axis from it, which `Seek` reads as the terrain slope for the speed correction (`k = LERP(-slope, 1.0, m_SpeedSlopeCorrDown)`). Robot `id=1262` was parked on a slope, so its speed drifted apart while every other field still matched. `CMatrixRobot::BeforeDraw` (a frame-time call) was also the only place that rebuilt that matrix, so the rebuild cadence followed the frame rate too.
5. **A per-frame cache shifted a logic value.** `RecalcTerainColor` (visual: it only feeds `D3DRS_TEXTUREFACTOR`) calls `CalcBounds`, which caches the bounding box for 100 ms. Frames therefore moved the moment at which `JoinToGroup` sampled the box, and with it `m_Core->m_GeoCenter` — which the logic does use (object search, damage). `SRemindCore` had the same shape: object resources were extended only in `BeforeDraw`, so unloading and reloading a graph reset animations at frame-dependent moments.
6. **The game menu consumed the logic RNG.** The planetary-battle launcher renders a preview battle in the same process and calls `rand()` there, so the stream had already advanced by an unpredictable amount before the fight started: two runs of the same map produced different troops (a robot with 4 weapons instead of 5). The harness now reseeds in `MatrixGameInit`, which runs once per battle.
7. **Open: the `BeforeDraw` pass still runs on frames.** It is entered from `CFormMatrixGame::Draw` before the code switches to the visual RNG phase, so it consumes logic-stream numbers and can create effects (burning-building smoke) at frame-dependent moments; those effects then deal damage from their logic takt. A draw-enabled run still diverged once at 25.4 s (one map object burned down in one run and survived in the other) while a `MATRIXGAME_NODRAW` run was identical. The fix is Phase 1 work: move effect spawning and the rest of the simulation-relevant part of `BeforeDraw` into the tick.

Fields that were provably read before being written and are now initialised: `CMatrixRobotAI` — `m_MovePathDist`, `m_MovePathDistFollow`, `m_MovePath[]`, `m_MoveTestPos`, `m_MoveTestChange`, `m_ZoneNear`, `m_NextTimeAblaze`, `m_NextTimeShorted`; `CMatrixRobot` — `m_BotRotAngle` (dead field, but it was full of garbage), `m_TimeWithBase`, `m_MaxHitPointInversed`. `SWeaponRepairData::m_Flags` still only has one bit cleared on allocation.

Current state: two runs of the same battle with `MATRIXGAME_FILL=CD` and `MATRIXGAME_FILL=AA` produce an identical world hash at every checkpoint from `t=0` to the end of the battle (`t=35000` on `armagedd` with seed `12345`), and so do two runs with no fill at all, i.e. with fresh allocations left untouched. A drawn pass and a no-draw pass of the same battle hash identically too. The hash line also carries the player-side status and the number of active sides, because the battle can end without the world hash ever differing.

Causes 4-6 live behind `MATRIXGAME_LOCKSTEP`, a CMake option that is on in every harness build and can be switched on in a plain build (`-DMATRIXGAME_LOCKSTEP=ON`, no instrumentation). In that mode: `SortEndGraphicTakt` walks the logic list instead of the camera-filtered `objects[]`, every object's core matrix is rebuilt at the end of the tick, object resources are extended per tick, and `SRemindCore::Takt` is driven by the logic step instead of frame time. Single player leaves the option off, because keeping every object's graphics resident costs memory; a network build needs it on. The terrain-colour pass was fixed unconditionally: it now takes the centre from the object core instead of calling `CalcBounds`, so frames can no longer shift the 100 ms bounds cache that `JoinToGroup` fills `m_GeoCenter` from.

The simulation now advances by whole `LOGIC_TAKT_PERIOD` (10 ms) steps in every build: `CFormMatrixGame::Takt` accumulates the measured frame time and runs a whole number of logic ticks (capped at eight, so a hitch cannot teleport the battle). `MATRIXGAME_FIXEDSTEP` still overrides the measured step for the harness, and with `MATRIXGAME_NODRAW=1` it must be set: without drawing the main loop spins with a measured step of about zero and the battle never advances. Verified: a drawn run driven by the wall clock (no fixed step) and a drawn run with `-Step 10` produce identical hashes at all 41 checkpoints over 20 s, even though their frame and visual-RNG counts differ.
Note that the camera still moves differently between runs (`m_CamDistSq` differs from the first checkpoint, because the frustum center is only refreshed in `BeforeDraw`), and the visual `rand()` stream differs too. Neither reaches the simulation any more; if a future change adds a camera or frame-rate dependency to logic, `camsq` in the object dump is the first thing to look at.

One practical trap: the game needs the right Steam account signed in. Without it `Rangers.exe` starts, exits immediately with code 0 and writes nothing at all.

Verified runs (24.11.2026, harness build, `MATRIXGAME_NODRAW=1` unless said otherwise):

| Map | Seed | Battle ends | Objects | CD/AA | No fill | Drawn |
| --- | --- | --- | --- | --- | --- | --- |
| `armagedd` | 12345 | 35 s | 67 | identical | identical | identical |
| `armagedd` | 777 | 38.5 s | 66 | identical | identical | not run |
| `bigcrater` | 4242 | 6 s | 59 | identical | identical | not run |
| `islands` | 31337 | 1 s | 75 | identical | identical | not run |
| `crossfire` | 4242 | 180 s (limit) | 87-121 | identical (181 pts) | identical | not run |

Auto-play battles end quickly: the AI resolves the map in 1-55 s, so the «120 s of game time» from the acceptance list is not reachable on this build — the longest map found is `crossfire` at 55 s with 121 objects. Use several maps and seeds instead of one long run.

One flake worth knowing about: the very first pass after the DLL was swapped once reported one extra object at the first checkpoint and never reproduced. `tools/Check-Determinism.ps1` retries a failing pair once for that reason, and a real desync reproduces every time.

A run can also stop because something crashed, not because the battle finished: `protectedBlock1` in `MatrixGameDll.cpp` catches the exception and exits quietly, so `MATRIXGAME_TRACE` plus `bwd=`/`xl=` in the hash line is the only way to tell the difference. That is how the first catch was found: `crossfire` died with `0xc0000005` inside `DumpObjects` after about 51 s. The faulting read was `m_Unit[u].m_WeaponRepairData->m_Flags` — the engine leaves that pointer dangling after `SWeaponRepairData::Release` (a real latent bug, the harness was simply the first reader), and the dump no longer follows it. With that fixed, `crossfire` runs the full 180 s of game time and both pairs match over 181 checkpoints.

The harness lives in `MatrixGame/src/SimHarness.{hpp,cpp}` and is inert unless `MATRIXGAME_SIM_HARNESS` is defined. Build it with:

```bat
cmake -S . -B build-sim -A Win32 -DMATRIXGAME_SIM_HARNESS=ON ^
  "-DCMAKE_CXX_FLAGS=-DWINDOWS_IGNORE_PACKING_MISMATCH" ^
  "-DCMAKE_C_FLAGS=-DWINDOWS_IGNORE_PACKING_MISMATCH"
cmake --build build-sim --config Release
```

A plain deterministic build (same lockstep behaviour, no instrumentation) is the same command with `-DMATRIXGAME_LOCKSTEP=ON` and a different build directory; the harness option implies the lockstep one. The harness option also enables `/MAP`, so RNG call sites reported as module offsets can be resolved to function names.

Environment switches:

| Variable | Meaning |
| --- | --- |
| `MATRIXGAME_SIM` | autoplay: skip the menu, start the battle |
| `MATRIXGAME_SEED` | seed for both streams instead of `time(NULL)` |
| `MATRIXGAME_FIXEDSTEP` | hard logic step in ms instead of the measured one |
| `MATRIXGAME_MAXTICKS` | quit after this much game time, ms |
| `MATRIXGAME_MAP` | force a map, ignoring the menu |
| `MATRIXGAME_HASHLOG` | world hash (and RNG counters) every `MATRIXGAME_HASHSTEP` ms |
| `MATRIXGAME_HASHSTEP` | log period, default 100 ms; 10 gives per-tick resolution |
| `MATRIXGAME_OBJLOG` | per-object field dump, keyed by stable id |
| `MATRIXGAME_RANDLOG` | `rand()` calls per return address, as a per-period delta |
| `MATRIXGAME_FILL` | byte used to fill fresh allocations (hex). Absent = allocations are left untouched, which is the real behaviour; setting it turns on the differential mode |
| `MATRIXGAME_POISON` | bytes of stack to poison with the same byte, 0 = off |
| `MATRIXGAME_NODRAW` | skip rendering entirely: the same battle runs an order of magnitude faster, which is what long determinism runs need |
| `MATRIXGAME_TRACE` | init stages and the crash address |
| `MATRIXGAME_TRACEBOT` | trace one object by id: Seek inputs, normal smoothing, matrix rebuilds |
| `MATRIXGAME_TRACEFROM` / `MATRIXGAME_TRACETO` | game-time window for that trace, ms |
| `MATRIXGAME_TRACEBOTLOG` | where the trace goes, default `tracebot.log` |
| `MATRIXGAME_CMDLOG` | write the command stream to this file (it doubles as a replay) |
| `MATRIXGAME_CMDREPLAY` | load a command log and apply it instead of generating commands |
| `MATRIXGAME_CMDBOT` | scripted player: issue an order for the player side every N ticks |
| `MATRIXGAME_CMDBOT_ALL` | with `CMDBOT`: the bot plays every human-controlled side (offline reference for a two-sided match) |
| `MATRIXGAME_NET` | enable the UDP layer (phase 3) |
| `MATRIXGAME_NET_PORT` / `MATRIXGAME_NET_BIND` | socket port and bind address; loopback by default |
| `MATRIXGAME_NET_PEER` | `host:port` — this process greets that host instead of listening |
| `MATRIXGAME_NET_SIDES` | which sides are human-controlled; must be identical on both machines |
| `MATRIXGAME_NET_LOCAL_SIDE` | id of the side this process plays |
| `MATRIXGAME_NET_HASH_PERIOD` | ticks between world-hash reports, default 50 |
| `MATRIXGAME_NET_WAIT_MS` | how long to wait for the peer before falling back to an offline run |
| `MATRIXGAME_NET_STOP_ON_DESYNC` | stop the match at the first hash mismatch |
| `MATRIXGAME_NETLOG` | net log: handshake, READY progress, `DESYNC` lines |
| `MATRIXGAME_NETHASHLOG` | net world hash per period; written without a socket too, which is what makes an offline run the reference |

The differential technique: run the same battle twice with `MATRIXGAME_FILL=CD` and `MATRIXGAME_FILL=AA`. Anything the engine reads before writing it will differ between the two runs, so the first field that diverges is the uninitialised one. `tools/` has the whole loop:

| File | What it does |
| --- | --- |
| `tools/Run-Sim.ps1` | runs one pass with the right variables, clicks through the menu to start the battle and waits for exit |
| `tools/simdiff.py` | compares two hash logs or two object dumps, reports the first diverging field |
| `tools/randlog_sites.py` | compares RNG call-site histograms and names the function via `MatrixGame.map` |
| `tools/gen_layout.py` | regenerates `SimLayout*.inc`, the field-offset tables the fill scanner uses |

`MATRIXGAME_OBJLOG` also reports, per robot, `garb=` (how many dwords still hold the fill byte) and `gof=` (their offsets, resolvable through the `.layout` file written next to the log). That is how the list of uninitialised fields above was produced.

A run is started by `tools/Run-Sim.ps1`, which puts the harness DLL into the installed game and drives the menu: the planetary battle module only starts when the player picks it there, so a bare `Start-Process Rangers.exe` sits in the main menu and writes nothing. `-AutoStart` (the default) clicks the bottom-left «ПЛАНЕТАРНЫЕ БОИ» icon and then «СТАРТ»; the coordinates are fractions of a 2560x1440 screen, so on another resolution recalibrate them or pass `-AutoStart:$false` and drive the menu by hand. Wall-clock cost is roughly 30-60 s per second of game time, so a 30 s battle takes about half an hour of real time per pass.

### Command layer (Phase 2)

Input no longer touches the world. Right-click orders, the keyboard and interface auto-order buttons and the group `STOP` build an `SCommand` (`MatrixGame/src/CmdQueue.{hpp,cpp}`) and push it into a queue; the queue is applied at the start of logic tick `N + 3` (`CMD_INPUT_LAG_TICKS`, 30 ms), before the side takts. Both machines therefore execute an order at the same tick, and commands due on the same tick run in `(tick, side, seq)` order.

A command carries the side id, the map-cell point, the target object's stable id, flags, four integer parameters, and the list of selected robot ids. The selection is part of the command because group assignment is a simulation action: `CMatrixSideUnit::SelGroupToLogicGroup` reassigns `m_GroupLogic` of the selected robots and used to run inside the input handler, i.e. at frame time. It now runs when the command is applied (`CmdAssignGroup` in `CmdQueue.cpp`), from ids stored in the command. That also removes a real source of drift: two identical clicks could previously land in different logic groups depending on how many frames had passed.

Building a robot is a command too (`CMD_BUILD_ROBOT`): the payload is a robot spec — chassis, armor, head, five weapon kinds and a count — and the apply step runs the same `CConstructor` the AI uses (`OperateUnit` per slot, then `GetConstructionPrice`, resource and robot-limit checks, then `StackRobot`). The "build" button in the constructor panel no longer builds on the spot: `CConstructor::RemoteBuild` creates that command, so the robot appears on tick `N + 3` and on both machines.

`MATRIXGAME_CMDLOG` writes the stream as text — a header (`v`, `map`, `seed`, `lag`) and one line per command. The same file is the replay: `MATRIXGAME_CMDREPLAY` loads it and applies the same commands at the same ticks, and `CmdIssue` refuses to add anything new in that mode. The match seed is no longer `time(NULL)` buried in init: `MatrixGameInit` calls `RndSetMatchSeed` with the seed chosen by the host (the harness takes `MATRIXGAME_SEED`) and writes it into the log header, where a replay checks it.

To exercise the layer without a human, the harness has a scripted player: `MATRIXGAME_CMDBOT=<ticks>` makes it order the player side every N ticks (attack the k-th live enemy, move, or stop) through the very same `CmdIssue` path. In that mode the player side keeps the old AI economy — the bot queues a robot into the base every 25 ticks through the constructor, because building as a command is not implemented yet — while the army moves only on commands. Run both passes with `-CmdBot` so the economy fixture makes the same calls in each.

```bat
tools\Run-Sim.ps1 -Name p2a -Map armagedd -Seed 12345 -Ticks 60000 -HashStep 500 -NoDraw -CmdBot 50
tools\Run-Sim.ps1 -Name p2c -Map armagedd -Seed 12345 -Ticks 60000 -HashStep 500 -NoDraw -CmdBot 50 ^
                  -CmdReplay tools\logs\p2a-cmd.txt
python tools\simdiff.py tools\logs\p2a.txt tools\logs\p2c.txt
```

Result (24.11.2026, 60 s of game time, 116 commands, 121 checkpoints): `armagedd`/12345 — two generation passes are identical (`6328608142b739b2`) and the replay of the first one is identical to both; the two command logs are byte-identical. `crossfire`/4242 — replay identical to generation (`eac52acc462d8868`).

Still outside the stream (the next step of Phase 2): building robots and turrets (`CConstructorPanel`, `m_CurrentAction == BUILDING_TURRET`), flyer orders (`CMatrixFlyer::SetTarget` runs at frame time and consumes logic-RNG numbers there), entering/leaving a robot (arcade mode) and the multi-selection UI state.

### Netplay layer (Phase 3)

`MatrixGame/src/NetPlay.{hpp,cpp}` is a UDP layer with one socket per process, enabled by `MATRIXGAME_NET=1`; by default it binds `127.0.0.1:27777`, and `MATRIXGAME_NET_BIND` opens it to the network. The peer address is learnt from its `HELLO`, so nothing has to be configured on the host side. Messages: `HELLO`/`HELLO_ACK` (side and match seed, rejected on seed mismatch), `CMD` (a numbered batch of commands), `ACK` (contiguous prefix received + how far this side has simulated), `READY` (the highest tick for which the sender promises to have sent everything) and `HASH`.

Three properties make it lockstep rather than state streaming:

1. **Delivery.** A batch stays in the retransmit queue until the peer's contiguous `ACK` covers it, and is resent every 100 ms otherwise. Duplicate batches are dropped by number, and the `ACK` only confirms a contiguous prefix — otherwise a lost batch would be "confirmed" by the next one and its commands would silently vanish.
2. **Tick barrier.** `CMatrixMapLogic::Takt` refuses to start tick `T` until `READY >= T` arrives, and `READY` is only sent once the peer has acknowledged every command up to that tick. Until the handshake completes the simulation stays at `t=0`; after `MATRIXGAME_NET_WAIT_MS` (60 s) it falls back to an offline run so a broken test does not hang.
3. **Hash exchange.** Every `MATRIXGAME_NET_HASH_PERIOD` ticks (50 by default) a world hash (objects, positions, hit points, groups, game time) is sent; a mismatch is written to `MATRIXGAME_NETLOG` as `DESYNC tick=... our=... peer=...`, and `MATRIXGAME_NET_STOP_ON_DESYNC=1` ends the match there.

Testing uses `tools/netpeer.py`, a Python peer that can play a side from a command log, only listen, drop every Nth batch, or send a deliberately wrong hash:

```bat
:: 1. offline reference: the same commands, no socket, and the net hash sequence
tools\Run-Sim.ps1 -Name p3ref -Map armagedd -Seed 12345 -Ticks 60000 -HashStep 500 -NoDraw -CmdBot 50 ^
                  -NetHashLog tools\logs\p3ref-net.txt

:: 2. the peer plays side 1 from that command log
python tools\netpeer.py --seed 12345 --side 1 --cmds tools\logs\p3ref-cmd.txt ^
                        --ref tools\logs\p3ref-net.txt --pace ack --end-tick 6000

:: 3. the game, which must reproduce the offline world bit for bit
tools\Run-Sim.ps1 -Name p3net -Map armagedd -Seed 12345 -Ticks 60000 -HashStep 500 -NoDraw -CmdBot 50 -Net
python tools\simdiff.py tools\logs\p3ref.txt tools\logs\p3net.txt
```

Verified (24.11.2026): 115 commands applied over UDP, 121 checkpoints identical to the offline run (`6328608142b739b2`), 120/120 net hashes matched. The reverse direction (the game plays, the peer only listens with `--echo --expect <the game's own cmd log>`) delivered 116/116 commands identical field by field. With `--drop-every 3` the peer loses 38 first transmissions, all are retransmitted, and both the world and all 120 hashes still match — that test also exposed a real bug: a late batch used to be queued out of order and its commands were applied on the wrong ticks (divergence at 5.5 s); `CmdTick` now scans the whole queue and sorts only what is due.

Two-sided match on one machine: an offline reference where the bot commands both human sides (`-CmdBotAll`) hashes `271185c6698a725a` with 712 commands (232 army orders + 480 robot builds), and a networked run where side 1 plays locally and side 2's orders *and builds* arrive over UDP reproduces it bit for bit (120/120 net hashes).

Still open: a match between two simulations. The code side is ready — `CMatrixSideUnit::m_HumanSide` decouples "this side is played by a human" from "this is the player side" (without it the second human side falls into the AI branch and the two machines diverge immediately), `MATRIXGAME_NET_SIDES` carries the list of such sides, and `MATRIXGAME_NET_PEER` turns a process into the joining client. Verified on one machine: an offline reference where the bot commands both human sides (`-CmdBotAll`) hashes `f6f085847ea64937`, and a networked run where side 1 is played locally and side 2 arrives over UDP reproduces it bit for bit (230 commands applied, 120/120 net hashes). What is missing is a second computer: `Rangers.exe` enforces a single instance ("Please terminate already running instance of the game!"), and that check is not path-based, so a junction to the same install does not help. Reconnect (catch up by replaying commands from the last checkpoint) and link emulation with latency and jitter are also still to do.

## 9. Pre-commit checklist

1. Builds as `Release` and `Debug` x86 with MSVC (there is no other check — the project has no tests).
2. `clang-format` applied to the changed files.
3. No edits under `ThirdParty/`, no reformatting of unrelated files.
4. No new `new`/`delete`/`malloc` bypassing `CHeap`.
5. `#include` case matches the actual file name.
6. Commit message is short and in the style of the history (`Fix character case of includes`, `Slightly change CMakeLists for mingw`).
7. If you touched movement, collision or AI code, re-run the determinism harness: two runs of the same battle must produce identical hash logs, and the `CD`/`AA` pair must stay identical too.
