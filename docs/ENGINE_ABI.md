# Export/ABI notes for reviewers

This project targets the MSVC **x86** ABI. The following exports were observed in the supplied `dinput8.dll.c` disassembly and are resolved by `GetProcAddress` at runtime:

| Engine.dll export | x86 method ABI | Purpose |
|---|---|---|
| `?AddLight@Entity@@QAEXPAVLight@@@Z` | `thiscall(Entity*,Light*)` | Add pointer to per-entity registry |
| `?ResetLights@Entity@@QAEXXZ` | `thiscall(Entity*)` | Clear collection |
| `?Draw@WorldMesh@@UAEXPAVRenderDevice@@H@Z` | `thiscall(WorldMesh*,RenderDevice*,int)` | Main draw stage |
| `?RenderLightPass@WorldMesh@@AAEXPBVLight@@@Z` | `thiscall(WorldMesh*,const Light*)` | Single light |
| `?RenderLightPass@WorldMesh@@AAEXPAVLight@@@Z` | `thiscall(WorldMesh*,Light*)` | Alternate constness export |
| `?RenderUberLightPass@WorldMesh@@AAEXQAPBVLight@@H@Z` | `thiscall(WorldMesh*,const Light**,int)` | Batch light pass |
| `?GetCurrentTimeMS@SystemDriver@@QBEKXZ` | `thiscall(SystemDriver*)` | Timer repair |

### Inferred, not proven for arbitrary builds

- `WorldMesh::Draw()` flags: `0x40000000` (lighting path).
- `Light+0x18`: flags, bit `0x400000` used to classify dynamic lights.
- `Light+0x7E0`: additional/special-light flag.
- `Light+0x7F4`: light type, `2` uses a single-light pass.
- `WorldMesh+0x7E8` and `+0x7EC`: internal special light pointers.
- `SystemDriver+0x50`: floating-point reciprocal time scale.
- Original patch uses 3-light uber passes in PK Black, 4-light uber passes in the Overdose family.

**Risk**: same export names do not ensure the fields retain these offsets. Future release should replace heuristic variant detection with hash-based tested profiles and add real frame captures as regression tests. A field offset error can crash the game.

The included source code was rewritten; the supplied third-party decompilation is **not** redistributed.
