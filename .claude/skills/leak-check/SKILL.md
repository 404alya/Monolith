---
name: leak-check
description: Run and read monolith's leak check (tools/leakcheck.sh, LeakSanitizer / ASan builds, tools/lsan_report.py, tools/lsan.supp). Required after any change to C, shaders or the build before the change counts as done; not for map-only edits. Use when finishing a code change, when leakcheck reports a leak, or when changing what it covers.
---

# Leak check

**The rule** (`CLAUDE.md`): after any change to code (C, shaders, build), `tools/leakcheck.sh` must end with `leakcheck: OK: zero leaks of ours` before the change counts as done. Map-only edits (including a package's own shaders) skip it; check a changed shader with `glslangValidator` instead (`docs/knowledge/r3d-pitfalls.md` §4). After larger changes also run `tools/leakcheck.sh --asan`.

```bash
./build.sh lsan                 # -O1 -g + LeakSanitizer -> build/linux-lsan/; examples: examples/build.sh lsan
tools/leakcheck.sh [--asan]     # every main code path under LSan (or ASan + UBSan); exit 1 on a leak
```

It takes about 1.5 minutes and runs everything in hidden windows (never on the developer's screen). Its test bundles go to its own requests folder, so it doesn't touch the queue. If it prints a leak, fix it before reporting the change as done (or before writing `result.md` for a request).

**Leak check** (`tools/leakcheck.sh`, after every code change, not map-only edits). Builds `build/linux-lsan/monolith` and `examples/build/linux-lsan/` when a source is newer, then runs every main path in hidden windows or none: the dump modes, shots of the default map (incl. `--shadow-camera --fog-cache`), `tests/models/`, `examples/01_minimal/scene` and `maps/pine_forest`, render scale, `--ao baked` / `--ao-volume` (also across a reload), `--reload-test` both ways and into a broken map, a broken map on the command line, `--shot-hud` with `--select` / `--region`, `--submit` on both maps with the stub terminal, `--export`, `--inspect-model`, `--new-map`, `--import`, `--classify`, every example's `--shot` and the capture shim. Created bundles, exports and packages go to the trash. Logs: `build/leakcheck/latest/logs/`. `tools/lsan_report.py` ignores a leak only when the frame that **allocated** it is in a driver or system library listed in `tools/lsan.supp` (GL drivers, libglvnd, X11/xcb, D-Bus, the loader); anything allocated in the monolith binary (incl. raylib, GLFW, R3D, assimp: all static) counts. On the dev machine the only ignored leak is NVIDIA's (8 allocations, 366 bytes, context creation). `--bench` adds a 10-frame headless bench (opt-in: one benchmark at a time, the `benchmarking` skill); `--asan` also fails on any sanitizer error. GL objects never deleted are not heap leaks, so `Unload*` calls still need review.

