# Light v0.1.7 — rename-only release

This release renames the v0.1.6 PainPatch project to **Light** without changing its dynamic-light behavior. Original Painkiller uses `dinput8.dll` + `Light.ini`; Custom EXE uses `Light.dll` + `Light.ini`. Runtime logs now go to `Light.log` (`[Light]` prefix). The preferred explicit initializer is `Light_Initialize`; the legacy `PKPatch_Initialize` export is retained for backward compatibility. Remove the old `PainPatch.dll` and `PainPatch.ini` when migrating; do not load both DLLs.

---

# Light v0.1.7 — CPU rendering optimization (experimental)

This source release moves per-light classification from `WorldMesh::Draw` to `Entity::AddLight`, bypasses D3D9 state/device setup for a single Uber light pass, uses a cached device lookup, skips empty-registry locks, and avoids diagnostic atomics when `LogLights=0`. These are targeted CPU optimizations, **not proof of a performance fix**.

Compile on Windows x86 using `build-win32.bat` (Visual Studio 2022). Look for the `v0.1.7 initializing` marker in Light.log after deployment. One live test should compare `RenderExtraLights=0` versus `RenderExtraLights=1` on exactly the same scene. Disable `LogLights` while measuring FPS. If graphical state leakage appears, set `FastSingleUberPass=0`. If engine pointer layouts differ, set `TrustEnginePointers=0` or disable the patch.

Light passes *must still run* per frame. Hooks and static D3D discovery can be initialized once; moving dynamic-light rendering to startup would not update illumination.

**Verification:** platform-independent CTest passed; Windows code has only been checked against local API header stubs. Real MSVC linkage, FPS, compatibility and graphical correctness remain unverified.

# Light (open-source DX9 lighting extension)

**Important:** This is *independently written* x86 C++ code derived from behavioral analysis of the supplied erychpatch DLL, **not** the original author's source code. It has **not yet been compiled under Windows or tested in Painkiller itself** in this environment. It is a working implementation *attempt*, not a verified drop-in replacement. Don't advertise it as a tested release without testing the x86 binaries in the game.

## What is implemented

- Real `dinput8.dll` proxy: forwards `DirectInput8Create` and the four COM helper exports to the system32 DLL, without recursing into itself.
- `Light.dll`: alternative for a custom `Painkiller.exe` to load with `LoadLibraryA`; C ABI export `Light_Initialize` for explicit init **after** Engine.dll is loaded. Both variants auto-start a short engine-discovery worker on loading.
- Engine.dll export detection and MinHook trampoline hooks for `Entity::AddLight`, `Entity::ResetLights`, `WorldMesh::Draw`, `WorldMesh::RenderLightPass` and `RenderUberLightPass` if present.
- Per-entity capped, deduplicated light registry, original Draw with suppressed stock 4-light paths, plus reissued single-light and uber-light passes (3/4 lights per batch); basic D3D9 additive blending, state restore, and optional `SetRenderState` interception.
- Optional RDTSC-based 64-bit divider patch for `SystemDriver::GetCurrentTimeMS` on PK Black variants when that symbol is present.
- `Light.ini`, runtime logs in `Light.log`, tests for light registry, GitHub Actions Windows x86 automated builds.

## Not yet implemented / validated

- `Game:Print()` to console or OutputDebugString, menu version label, PNG screenshots, and the old windowed/borderless mode hook. These were found in the reference plugin but **are not implemented in this preview**.
- Accurate validation of engine binary layouts, draw flags and per-light filters in all Painkiller editions. The original decompilation is imperfect and cannot prove binary compatibility.
- Game-level run or renderer-frame comparison. DX11 D3Dev11.dll is **not supported**; this is a DX9 patch.
- Collaboration with or consent from the original creator of erychpatch to redistribute their code. Only independently written code is included here.

## Building on Windows (Visual Studio 2022, x86)

1. Install Visual Studio 2022 with **Desktop development with C++** and CMake support; Internet needed for CMake to fetch BSD-licensed MinHook v1.3.4 from the official repo.
2. At the repository root:

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
ctest --test-dir build --config Release --output-on-failure
```

Output binaries: `build\release\Release\dinput8.dll` and `Light.dll`. For CI builders, open the GitHub repository > Actions > **Build Windows x86 patch** > Run workflow, then download its `Light-Win32` artifact. The project has no Windows build artifacts checked in.

## How to load

**Original game (32-bit D3D9):** place `dinput8.dll` and `Light.ini` beside the original `Painkiller.exe`. **Back up/remove any existing dinput8.dll first**; two different libraries cannot occupy the same filename. Launch the game and inspect `Light.log` for `lighting ACTIVE`. If your game does not import `dinput8.dll`, the proxy will not load and you need an alternate injection mechanism.

**Custom EXE:** avoid the `dinput8.dll` filename conflict. Put `Light.dll` and `Light.ini` next to your EXE, add the following after Engine.dll has loaded:

```cpp
HMODULE p = LoadLibraryA("Light.dll");
if (p) {
    using Init = BOOL (__cdecl*)();
    auto init = reinterpret_cast<Init>(GetProcAddress(p, "Light_Initialize"));
    if (init) init();
}
```

`Init()` returning TRUE means it installed at least one hook, **not** that all features are active. See the log for `dynamic lighting ACTIVE` and `timer ACTIVE`. Keep this DLL loaded for the lifetime of the process; don't call `FreeLibrary` on it in-game.

> If the custom EXE includes a modified or recompiled Engine.dll, it must expose compatible x86 MSVC decorated names and memory layout. If the custom EXE has a rewritten DX11 renderer, additional testing/porting is required.

## Configuration

See `config/Light.ini`. `MaxLights=128` is **per entity/mesh**, not a global light count. More light passes cost more GPU time. For first integration tests, use 8 or 16 and run the same scene with / without the DLL.

## First-run checklist

1. Use a disposable installation/copy and verify the SHA of Engine.dll. Do not test against someone's unique modding environment first.
2. Verify that disabling `UnlockLights=0` restores vanilla rendering.
3. Confirm the log says **Installed** and **dynamic lighting ACTIVE**. A `missing export` or `MH_CreateHook failed` indicates incompatible Engine.dll.
4. Place 5, then 8, 16, 32 and 128 dynamic lights on one mesh. Visually compare with stock rendering and monitor FPS and texture/transparency artifacts.
5. Try `HookD3DState=0` if an existing DX9 wrapper/overlay conflicts; this may change multi-pass blending.
6. If there is a crash or corrupted lighting, remove dinput8.dll / disable `UnlockLights` and attach `Light.log` to a bug report.

## Licence and provenance

New source code in this repository: MIT (see `LICENSE`). MinHook is fetched separately and uses its own BSD licence. The reference erychpatch author retains rights to their original code. This project does not bundle the binary or Hex-Rays source dump.

### One-command build

Run `build-win32.bat` from a Visual Studio 2022 Developer Command Prompt or a normal `cmd.exe` with CMake on PATH. A successful build populates `dist/vanilla/` and `dist/custom-exe/`, including the relevant `.ini` file and MinHook licence notice. The script exits with an error if DLLs are missing.

## v0.1.1 export hotfix

Fixes x86 MSVC `LNK2001 unresolved external symbol _PKPatch_Initialize`
from both `.def` files by exporting `Light_Initialize` without a
redundant underscore-prefixed internal alias.

This is a linker fix only. The resulting DLLs have not been game-tested.


## Experimental v0.1.5 — FPS regression investigation

Build with `build-win32.bat` and compare three clean-launch runs in the **same** scene:

1. **Baseline with no hooks:** `UnlockLights=0`, `TimerSizeFix=0`, `LogLights=0`.
2. **Hooks only (no new passes):** `UnlockLights=1`, `RenderExtraLights=0`, `TimerSizeFix=0`, `LogLights=0`.
3. **One light pass budget:** `UnlockLights=1`, `RenderExtraLights=1`, `MaxLights=8`, `MaxRenderLightsPerMesh=4`, `MaxExtraPassesPerMesh=1`, `TimerSizeFix=0`, `LogLights=0`.

Re-launch between configurations. Next enable `LogLights=1` in mode 3 and collect `DIAG` lines from `Light.log`. These are *cumulative* counts, not per-frame metrics. In particular inspect `nonempty`, `maxSnapshot`, `deviceMissing`, `d3dScans`, `d3dNegativeCacheSkips`, `uberCalls`, and `singleCalls`.

Do not combine this DLL with erychpatch or any other patch hooking the same lighting functions.

This is unverified in-game experimental source code; running Windows MSVC and FPS tests is still required.


## Model/PKMDL limitations

PKMDL-specific model rendering has **not** been verified. Light hooks currently operate on `Entity` and `WorldMesh` paths, not a dedicated PKMDL/skinned renderer. Read `KNOWN_LIMITATIONS.md` before advertising model support.
