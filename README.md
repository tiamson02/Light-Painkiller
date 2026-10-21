# Light v0.2.7 — experimental dynamic-light extension for Painkiller

Open-source light overflow extension for 32-bit DirectX 9 PainEngine (`Engine.dll`) and compatible mods. It attempts to illuminate world geometry with more than four contributing light sources per mesh.

**Rendering:** the implementation follows erychpatch's light-replay sequence: on eligible meshes with more than four registered sources, original light-pass calls are suppressed temporarily during `WorldMesh::Draw` and the selected light list is replayed after the draw. If replay planning cannot safely proceed, the original draw runs untouched. Meshes with at most four lights always use native rendering. The expanded light-capacity configuration has not yet been verified in PainEditor or RoE.

## Light.ini

```ini
[Settings]
UnlockLights=1
MaxLights=128
ExtraPassesPerMesh=124
TimerSizeFix=1
LogLights=0
```

- `UnlockLights` — enable light extension (`0` disables light interceptions).
- `MaxLights` — the maximum size of the source registry for Entity (4–512), **not** the number of actually rendered lamps.
- `ExtraPassesPerMesh` — budget for passes beyond the basic ones (0–512); `0` completely disables all lighting hooks.
- `TimerSizeFix` — independent timer fix. You can set it to `0` to test the light.
- `LogLights` — an aggregated diagnostic with an interval of approximately 5 seconds. `replayedMeshes` shows how many meshes were redrawn via the new path, `replayedNative` / `replayedOverflow` — the number of basic and additional sources in the selected plans.

`Light.log` is **overwritten once when the process starts** and is stored next to the loaded DLL.

## Build & install

On Windows with Visual Studio 2022 Win32 C++ tools and CMake, run `build-win32.bat`. Install **either** `dist/vanilla/dinput8.dll` for original Painkiller **or** `dist/custom-exe/Light.dll` for Custom EXE, with `Light.ini` next to the DLL. For a PainEditor install in `Bin\\Editor`, put `Light.ini` there as well.

## Validation

Compare identical scenes with the patch disabled and enabled, especially soul glow on world geometry, distant torches, projected flashlights and FPS. `Installed lighting=1` does **not** prove extra light was drawn: inspect counters with `LogLights=1`. Unit tests cover the light registry, light-replay planning and a mock WorldMesh hook path; they are not a substitute for a real Win32/MSVC build and in-game visual testing.

Projected lights, PKMDL lighting, render-state cache integration, compatibility with all Engine.dll variants and performance with very many sources are not fully verified. See `KNOWN_LIMITATIONS.md`.


**Budget semantics (v0.2.7):** `MaxLights` is the registry limit; `ExtraPassesPerMesh` limits extra **draw calls** per mesh. For 128 ordinary lights, up to 124 extra calls may be required. The default prioritizes displaying all registered lights over FPS. Lower the budget (for example, 16–32) only when accepting omitted light sources. No spatial culling is implemented yet.

## Custom EXE integration (v0.2.9)

`Light.dll` in `Data/addons` **building a personal `Painkiller.exe` for RoE**.
The EXE explicitly calls the DLL export `Light_Initialize()` synchronously
after loading it, before Lua and world startup. Unlike `dinput8.dll`, this
build does not install hooks from a background thread started in `DllMain`.
After installing `dist/custom-exe/Light.dll` alongside `Light.ini`, rebuild
and replace `Painkiller.exe` from the supplied EXE project. Watch for
`[Addons][Light] initialization=OK lightingActive=1` in the EXE log.
This confirms hooks are installed, **not** that lighting is visibly correct.
Do not run both this addon and a local `dinput8.dll` Light proxy at once.

