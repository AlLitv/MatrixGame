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

Three root causes of desync have been found so far; two of them are fixed structurally:

1. **Pointer comparisons used as tie-breakers.** `CollisionCallback` (`MatrixRobot.cpp`) decided which of two head-on robots yields via `DWORD(pCurrBot) < DWORD(data->robot)`. Heap addresses differ between runs, so the winner differed and the battle desynced after a few seconds. Map objects now carry a stable creation-order id (`CMatrixMapStatic::m_Id` / `GetId()`), and both comparisons use it.
2. **The logic AI and the renderer shared one RNG stream.** `RND`/`FRND`/`IRND` expand to `rand()`, and visual effects call them from `Draw`. Every extra draw (a visible repair beam, a dust puff, a debris look) shifts the sequence the AI consumes, so what is on screen desyncs what happens in the world. The harness now keeps two streams: logic takes numbers from its own xorshift (seeded from `MATRIXGAME_SEED`), the whole `CFormMatrixGame::Draw` phase takes them from `rand()`. This moved the first cross-fill divergence from 6100 ms to 9630 ms of game time and, more importantly, made visual noise unable to affect the simulation at all.
3. **Reading uninitialised memory.** A field read before being written returns heap garbage, which differs between processes. `CHeap::Alloc` zeroes its memory under the harness, which makes this class deterministic; the field-by-field cleanup is in progress.

Fields that were provably read before being written and are now initialised: `CMatrixRobotAI` — `m_MovePathDist`, `m_MovePathDistFollow`, `m_MovePath[]`, `m_MoveTestPos`, `m_MoveTestChange`, `m_ZoneNear`, `m_NextTimeAblaze`, `m_NextTimeShorted`; `CMatrixRobot` — `m_BotRotAngle` (dead field, but it was full of garbage), `m_TimeWithBase`, `m_MaxHitPointInversed`. `SWeaponRepairData::m_Flags` still only has one bit cleared on allocation.

What is left: at ~9630 ms the world diverges in `m_Speed` of one robot while **every** dumped field still matches and the logic RNG counters are identical, i.e. the differing input is not part of the robot object and not RNG. The differential harness is the tool to chase it (see below).

The harness lives in `MatrixGame/src/SimHarness.{hpp,cpp}` and is inert unless `MATRIXGAME_SIM_HARNESS` is defined. Build it with:

```bat
cmake -S . -B build-sim -A Win32 -DMATRIXGAME_SIM_HARNESS=ON ^
  "-DCMAKE_CXX_FLAGS=-DWINDOWS_IGNORE_PACKING_MISMATCH" ^
  "-DCMAKE_C_FLAGS=-DWINDOWS_IGNORE_PACKING_MISMATCH"
cmake --build build-sim --config Release
```

The option is a normal CMake switch now (it defines `MATRIXGAME_SIM_HARNESS` for every target, because the allocator in `MatrixLib` needs it too) and it also enables `/MAP`, so RNG call sites reported as module offsets can be resolved to function names.

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
| `MATRIXGAME_FILL` | byte used to fill fresh allocations (hex), default `00` |
| `MATRIXGAME_POISON` | bytes of stack to poison with the same byte, 0 = off |
| `MATRIXGAME_TRACE` | init stages and the crash address |

The differential technique: run the same battle twice with `MATRIXGAME_FILL=CD` and `MATRIXGAME_FILL=AA`. Anything the engine reads before writing it will differ between the two runs, so the first field that diverges is the uninitialised one. `tools/` has the whole loop:

| File | What it does |
| --- | --- |
| `tools/Run-Sim.ps1` | runs one pass with the right variables and waits for exit |
| `tools/simdiff.py` | compares two hash logs or two object dumps, reports the first diverging field |
| `tools/randlog_sites.py` | compares RNG call-site histograms and names the function via `MatrixGame.map` |
| `tools/gen_layout.py` | regenerates `SimLayout*.inc`, the field-offset tables the fill scanner uses |

`MATRIXGAME_OBJLOG` also reports, per robot, `garb=` (how many dwords still hold the fill byte) and `gof=` (their offsets, resolvable through the `.layout` file written next to the log). That is how the list of uninitialised fields above was produced.

## 9. Pre-commit checklist

1. Builds as `Release` and `Debug` x86 with MSVC (there is no other check — the project has no tests).
2. `clang-format` applied to the changed files.
3. No edits under `ThirdParty/`, no reformatting of unrelated files.
4. No new `new`/`delete`/`malloc` bypassing `CHeap`.
5. `#include` case matches the actual file name.
6. Commit message is short and in the style of the history (`Fix character case of includes`, `Slightly change CMakeLists for mingw`).
7. If you touched movement, collision or AI code, re-run the determinism harness: two runs of the same battle must produce identical hash logs, and the `CD`/`AA` pair must stay identical too.
