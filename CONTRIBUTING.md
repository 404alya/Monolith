# Contributing

The rules for changing monolith, whether you write the change yourself or with Claude Code. Claude follows the same rules from `CLAUDE.md` and the skills in `.claude/skills/`; this page states them for people. How the code is organised: [docs/architecture.md](docs/architecture.md).

## A code change is done when

1. **Both targets build with zero warnings:** `./build.sh` and `./build.sh --win`, plus `examples/build.sh` (and `--win`) when the runtime or an example changed. Changed shaders pass `glslangValidator` (Arch: `glslang`; Debian / Ubuntu: `glslang-tools`). A change to `runtime/` also builds without `MONOLITH_EDITOR` (an `--export` package builds it that way) and without `MS_PATCHED_R3D`.
2. **The leak check passes:** `tools/leakcheck.sh` ends with `leakcheck: OK: zero leaks of ours` (about 1.5 minutes, hidden windows only). After larger changes also `tools/leakcheck.sh --asan`.
3. **Every map looks the same**, unless changing the look is the point of the change: identical box lists (`--dump-boxes`) and pixel-identical `--shot` images of each map's viewpoints, before vs. after. A change that alters the look is presented as a trade-off (behind a setting, with before / after images) and needs the maintainer's approval.
4. **The tests that cover it pass:** `tools/test-coordinator.sh` for the launcher and request queue; the unit check in `tests/unit/wincmd_test.c` (its first line shows how to build it) for the Windows command line; `--reload-test`, `--shot` and the dump flags for the loader and renderer ([command-line.md](docs/command-line.md)).
5. **The docs say what the code does:** the header comment of `src/main.c` (controls and flags), the pages in `docs/`, and the skill in `.claude/skills/` whose workflow changed.

Map-only changes (a `.map` file, `MAP.md`, a package's shaders, models, textures) need none of the builds or the leak check: the game reloads the package while it runs.

## Rules

- **No fixed-size buffers for content.** Anything whose size depends on the map, the selection, the regions or the input is a heap array (`MS_RESERVE` / `MS_PUSH`) or a heap string. Fixed arrays only for stack scratch that formats one path or message. The per-frame path never allocates.
- **R3D changes are patches.** Work around R3D from the app side where reasonable. When R3D itself must change, change it minimally and record it in `submodules/r3d/patches.md` (symptom, cause, files, diff, verification).
- **Assets need a recorded licence:** CC0 / public domain, or CC-BY with the attribution. Every external file gets a row in its package's `ASSETS.md` and a `<file>.asset.md` sidecar (`tools/record_asset.py` writes both). Models are restyled with material overrides in the map, never by editing the file.
- **The level is data.** Nothing in the engine or the shared docs assumes one map's style; a map's style and rules live in its `MAP.md`.
- **Performance claims** come with interleaved A/B runs of the old and new binary on the same machine (`--bench --headless`, at least 3 pairs), reported as relative changes together with the machine (the bench report's GPU, driver and CPU). Numbers from another machine are not a baseline.
- **Automated runs never open a visible window:** use `--shot`, the dump flags, `--inspect-model` or `--bench --headless`. In an AI agent's shell the game and the examples enforce this (they refuse and exit 1 unless `MONOLITH_ALLOW_WINDOW=1`); keep new modes that show no window in `src/main.c`'s `windowless` check.
- **Don't commit** `build/`, screenshots, `benchmark/`, `requests/`, `export/` or your `.claude/settings.local.json`. Most maps in `maps/` are kept locally and are gitignored; only `maps/pine_forest/` is in the repository.

## Style

C11, 4-space indent, `static` file-local functions, PascalCase functions, `UPPER_CASE` macros, `// #region` sections, one module per concern with a header comment, short comments that explain *why*. Comment non-obvious engine workarounds with their reason so nobody "cleans them up". English everywhere.

## Docs

`README.md` and `docs/` are written for people: describe what the software does and how to use it, without instructions to an AI assistant or session history. Workflows and lessons for Claude Code go into a skill in `.claude/skills/`; measurements into `.claude/skills/benchmarking/measurements.md`, labelled with the machine.
