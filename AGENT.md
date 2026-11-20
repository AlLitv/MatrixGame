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

The simulation can be made deterministic, and **two root causes of desync have already been found and fixed**:

1. **Pointer comparisons used as tie-breakers.** `CollisionCallback` (`MatrixRobot.cpp`) decided which of two head-on robots yields via `DWORD(pCurrBot) < DWORD(data->robot)`. Heap addresses differ between runs, so the winner differed and the battle desynced after a few seconds. Map objects now carry a stable creation-order id (`CMatrixMapStatic::m_Id` / `GetId()`), and both comparisons use it.
2. **Reading uninitialised memory.** A field read before being written returns heap garbage that differs between runs. Under `MATRIXGAME_SIM_HARNESS` the engine allocator (`CHeap::Alloc`) zeroes the memory it hands out, which makes this whole class deterministic. The exact field is still unknown: it sits in the movement path, but it is none of the speed / slope-correction / forward / collision-weight fields.

The harness lives in `MatrixGame/src/SimHarness.{hpp,cpp}` and is inert unless `MATRIXGAME_SIM_HARNESS` is defined. Build it with:

```bat
cmake -S . -B build-sim -A Win32 ^
  "-DCMAKE_CXX_FLAGS=-DWINDOWS_IGNORE_PACKING_MISMATCH -DMATRIXGAME_SIM_HARNESS" ^
  "-DCMAKE_C_FLAGS=-DWINDOWS_IGNORE_PACKING_MISMATCH"
```

Environment switches: `MATRIXGAME_SIM` (autoplay, skip the menu), `MATRIXGAME_SEED`, `MATRIXGAME_FIXEDSTEP` (ms), `MATRIXGAME_MAXTICKS`, `MATRIXGAME_MAP` (force a map regardless of the menu), `MATRIXGAME_HASHLOG` (world hash every 100 ms), `MATRIXGAME_OBJLOG` (per-object dump), `MATRIXGAME_TRACE` (init stages and the crash address).

How to use it: put the harness DLL into Space Rangers HD, play the same battle twice, compare the hash logs. Identical logs mean the run was deterministic. `tools/start-battle.cmd` launches the game with the right variables.

## 9. Pre-commit checklist

1. Builds as `Release` and `Debug` x86 with MSVC (there is no other check — the project has no tests).
2. `clang-format` applied to the changed files.
3. No edits under `ThirdParty/`, no reformatting of unrelated files.
4. No new `new`/`delete`/`malloc` bypassing `CHeap`.
5. `#include` case matches the actual file name.
6. Commit message is short and in the style of the history (`Fix character case of includes`, `Slightly change CMakeLists for mingw`).
7. If you touched movement, collision or AI code, re-run the determinism harness: two runs of the same battle must produce identical hash logs.
