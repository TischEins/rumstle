# RUMSTLE — sources

VST3 instrument and Windows standalone, derived from the Bastl Kastle v1.5
firmware (Václav Peloušek / Bastl Instruments, CC BY-SA). End-user text,
attribution and licence: `LIESMICH.txt` (shipped with every release).

The folder and the engine files are still called `kastleport` / `kastle_*`;
only the product name changed.

| File | Purpose |
|---|---|
| `kastle_vco.*`, `kastle_lfo.*`, `kastle_noise_table.*` | C port of the two firmware chips |
| `kastle_vst3.*` | VST3 shell: parameters, patch layer, state, factory |
| `kastle_editor.*` | the whole faceplate as one custom view (layout tables at the top of the .cpp) |
| `kastle_standalone.cpp` | `RUMSTLE.exe`: SDK editorhost window + WASAPI output + "Set" menu |
| `resource/kastleport.uidesc` | bitmaps, colours and the Mode / XY / Mono strip |
| `make_art.ps1` | rebuilds `resource/*.png` from the artist's exports; run after any graphic changed |

## Build (Windows, Visual Studio 2019+, from the SDK root)

```
cmake -S . -B build_full -DSMTG_USE_STATIC_CRT=ON
cmake --build build_full --target rumstle_standalone --config Release
```

This builds the plugin, runs Steinberg's validator on it, and builds the
standalone with the bundle copied next to it (`build_full/bin/Release`).
`SMTG_USE_STATIC_CRT=ON` keeps the release free of the Visual C++
redistributable.

## Rules that keep old sessions loading

- New parameter ids go at the END of `KastleParamId`.
- New state fields go at the END of the stream in `getState`/`setState`; a
  failed read means "older file" and takes a default.
- Bump the version in `project(VERSION)` and in `DEF_CLASS2`.
