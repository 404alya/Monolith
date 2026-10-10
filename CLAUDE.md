# monolith

monolith is an **AI-assisted map designer** for brutalist concrete levels, written in plain C11 on raylib 5.5 + R3D 0.10.x (deferred PBR). The game is the editor and viewer. Levels are plain-text files of axis-aligned boxes (`maps/*.map`) that the game hot-reloads. In spectator mode the developer points at a spot, selects boxes and types a request. The game writes a bundle to `requests/<id>/` and opens a Claude Code session in a new terminal to carry the request out. If you were started for such a request, read `requests/<id>/request.md` and follow `AI-MAP-TOOL.md`.

@AI-MAP-TOOL.md
@PROMPT-INSTRUCTIONS.md

## Hard rules

- **Never take over the developer's screen.** Run the game only with `--shot <file>` (hidden window), `--dump-boxes`, or `--bench --headless`. Never open a visible window, and never switch i3 workspaces.
- **Benchmarks are noisy** (~10% swings between GPU clock states). Screen with `--bench-frames 150`, and report numbers only from full runs. Verify every claimed gain or regression with **interleaved A/B pairs** (A, B, A, B, … ≥ 3 pairs, two binaries side by side, same flags), with `__GL_SYNC_TO_VBLANK=0` and `--note "what changed"` on every run. Only one benchmark may run at a time. Map edits never need a benchmark.
- **The Bash tool runs zsh:** an unquoted `$VAR` does not word-split. Use arrays or inline env assignments (`__GL_SYNC_TO_VBLANK=0 ./build/linux/monolith …`).
- **Both targets build with zero warnings:** `./build.sh` and `./build.sh --win`, after any code change.
- **Don't change how the scene looks without the developer's approval.** The exception is when the look is what the request asks for. Optimisations must be pixel-identical (prove it with `--shot` comparisons), or be presented as a trade-off with before/after images.
- **Record every change under `submodules/r3d`** in `submodules/r3d/patches.md` (symptom, cause, files, diff, verification). Don't touch `~/Projects/r3d` or other projects.
- **Don't commit unless asked.** Never commit `build/`, screenshots, `benchmark/` or `requests/`.
