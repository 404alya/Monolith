# Contributing

The rules for changing monolith, whether you write the change yourself or with Claude Code. Claude follows the same rules from `CLAUDE.md` and the skills in `.claude/skills/`; this page states them for people. How the code is organised: [docs/architecture.md](docs/architecture.md).

## A code change is done when

1. **Both targets build with zero warnings:** `./build.sh` and `./build.sh --win`, plus `examples/build.sh` (and `--win`) when the runtime or an example changed. Changed shaders pass `glslangValidator` (Arch: `glslang`; Debian / Ubuntu: `glslang-tools`). A change to `runtime/` also builds without `MONOLITH_EDITOR` (an `--export` package builds it that way) and without `MS_PATCHED_R3D`.
2. **The leak check passes:** `tools/leakcheck.sh` ends with `leakcheck: OK: zero leaks of ours` (about 1.5 minutes, hidden windows only). After larger changes also `tools/leakcheck.sh --asan`.
3. **Every map looks the same**, unless changing the look is the point of the change: identical box lists (`--dump-boxes`) and pixel-identical `--shot` images of each map's viewpoints, before vs. after. A change that alters the look is presented as a trade-off (behind a setting, with before / after images) and needs the maintainer's approval.
4. **The tests that cover it pass:** `tools/test-coordinator.sh` for the launcher and request queue; the unit check in `tests/unit/wincmd_test.c` (its first line shows how to build it) for the Windows command line; `--reload-test`, `--shot` and the dump flags for the loader and renderer ([command-line.md](docs/command-line.md)).
5. **The docs say what the code does:** the header comment of `src/main.c` (controls and flags), the pages in `docs/`, and the skill in `.claude/skills/` whose workflow changed.

Map-only changes (a `.map` file, `MAP.md`, a package's shaders, models, textures) need none of the builds or the leak check: the game reloads the package while it runs.

## The build

`./build.sh` only compiles and starts the build program: `nob.c` in the repo root, a C program written on the vendored [nob.h](https://github.com/tsoding/nob.h) (`src/vendor/nob/`). Every mode, compiler flag and command of the build lives in `nob.c`, each command with a comment that says what it does and why. `build.sh` recompiles `build/nob` by itself when `nob.c` or `nob.h` changed. Each command is echoed (`[INFO] CMD: …`) and followed by its wall time and the CPU time it and its child processes used (`[TIME] …`); a summary table of all commands ends the build, so you can see where the time goes. A new flag goes into the argument list of the command it belongs to in `nob.c` (and into the matching `compile_commands.json` entry when that list is mirrored there); a new mode is a branch in `main()` / `BuildLinux()` that runs its commands through `RunTimed()`. Commands run one after another (that is what makes the CPU times exact). `examples/build.sh`, `tests/unit/*.sh` and the export's `build.sh` are still shell scripts.

## Rules

- **No fixed-size buffers for content.** Anything whose size depends on the map, the selection, the regions or the input is a heap array (`MS_RESERVE` / `MS_PUSH`) or a heap string. Fixed arrays only for stack scratch that formats one path or message. The per-frame path never allocates.
- **R3D changes are patches.** Work around R3D from the app side where reasonable. When R3D itself must change, change it minimally and record it in `submodules/r3d/patches.md` (symptom, cause, files, diff, verification).
- **Assets need a recorded licence:** CC0 / public domain, or CC-BY with the attribution. Every external file gets a row in its package's `ASSETS.md` and a `<file>.asset.md` sidecar (`tools/record_asset.py` writes both). Models are restyled with material overrides in the map, never by editing the file. The editor's own font (`resources/fonts/`) is the one exception: it may be under the SIL Open Font License 1.1, recorded the same way, with its `OFL.txt` shipped next to the binary.
- **The level is data.** Nothing in the engine or the shared docs assumes one map's style; a map's style and rules live in its `MAP.md`.
- **Performance claims** come with interleaved A/B runs of the old and new binary on the same machine (`--bench --headless`, at least 3 pairs), reported as relative changes together with the machine (the bench report's GPU, driver and CPU). Numbers from another machine are not a baseline.
- **Automated runs never open a visible window:** use `--shot`, the dump flags, `--inspect-model` or `--bench --headless`. In an AI agent's shell the game enforces this (it refuses and exits 1 unless `MONOLITH_ALLOW_WINDOW=1`); keep new modes that show no window in `src/main.c`'s `windowless` check. The examples are plain programs that always open a window; tests run their test build (`examples/build.sh test`, `tests/examples/harness.h`), which needs `--shot` and never shows one.
- **Don't commit** `build/`, screenshots, `benchmark/`, `requests/`, `export/` or your `.claude/settings.local.json`. Most maps in `maps/` are kept locally and are gitignored; only `maps/pine_forest/` is in the repository.

## Style

C11, 4-space indent, `static` file-local functions, PascalCase functions, `UPPER_CASE` macros, `// #region` sections, one module per concern with a header comment, short comments that explain *why*. Comment non-obvious engine workarounds with their reason so nobody "cleans them up". English everywhere.

## Editor setup

Code navigation (go to definition, find references, symbol search, call hierarchy) works with any editor that uses [clangd](https://clangd.llvm.org/), and follows calls across files and into R3D and raylib (`submodules/r3d/src/`, `submodules/r3d/external/raylib/src/`).

- **`./build.sh` writes `compile_commands.json`** in the repo root: one entry per file with the exact flags the build uses (includes, `-DMONOLITH_EDITOR`, `-DMS_PATCHED_R3D`, the path defines), so clangd sees each file as the compiler does and editor-only code isn't greyed out. It also covers `tools/profile.c`, the vendored xatlas, `examples/` and `tests/unit/` (with their scripts' flags), and R3D's and raylib's sources (from R3D's own CMake export; assimp is left out). Only the plain Linux build writes it; `debug`, `lsan`, `profile` and `--win` leave it alone. It needs `python3`.
- **When to regenerate:** after a fresh clone, after adding a `.c` file, and after a change to a flag in `nob.c` (the build program `./build.sh` runs): run `./build.sh` again. The file holds absolute paths for your checkout and is gitignored; never edit it by hand. Each git worktree gets its own.
- **Let clangd index in the background** (`--background-index`, the default in most setups): it can then jump into files you haven't opened. Its index goes into `.cache/clangd/` (gitignored).
- **No `.clangd` with `CompileFlags: Add:`** is needed. An old one adds flags on top of the database's (a second `-std`, stale defines): remove that block, or the file.
- **Neovim:** LSP go-to-definition is `gd` in LazyVim. Ctrl+click is a tags jump by default; map it to the LSP in your own config if you want it, e.g. `vim.keymap.set("n", "<C-LeftMouse>", "<LeftMouse><cmd>lua vim.lsp.buf.definition()<cr>")`.

## Docs

`README.md` and `docs/` are written for people: describe what the software does and how to use it, without instructions to an AI assistant or session history. Workflows and lessons for Claude Code go into a skill in `.claude/skills/`; measurements into `.claude/skills/benchmarking/measurements.md`, labelled with the machine.
