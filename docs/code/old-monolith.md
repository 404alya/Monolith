# `tools/old-monolith.c`: build an older monolith

`tools/old-monolith.c` builds monolith as it was at an older commit and prints the binary's path. It replaced `tools/old-monolith.sh` (2026-11-13) with the same arguments, folders, log, messages and exit codes. Overview of all the tools: [architecture.md §8](../architecture.md#8-tools).

## Usage

```
build/tools/old-monolith <commit>
```

The `migrate-map` skill uses it to build a map format's last reader (the `last-reader:` lines of `.claude/skills/migrate-map/versions.md`) and prove a migration against it.

## How it works

- **The commit** is resolved in the main repository (the checkout that owns this repo's git objects, also when the program runs from a worktree). Folders are named after its first 12 hex digits.
- **Reuse.** If `/tmp/monolith-old-<hash12>-<n>/` holds `.old-monolith-done` with the full hash and its `build/linux/monolith` exists, that binary is printed without building (the lowest such `n` wins).
- **Otherwise, a fresh build:**
  1. A fresh worktree `/tmp/monolith-old-<hash12>-<n>/` (the first `n` with nothing of that name, nor a `<n>.log`), made with `git worktree add --detach`.
  2. `submodules/r3d` initialised at the commit's gitlink from the main checkout's objects, when the commit has it as a submodule (no network; commits that vendor R3D as a plain tree need nothing).
  3. The commit's own `./build.sh`, with `R3D_DIR` unset (the commit's own R3D) and `PATH` led by the folder `<worktree>/.old-monolith-shim`, whose `rm`, `rmdir` and `unlink` are symlinks to this program (see [The delete stand-in](#the-delete-stand-in)).
  4. On success it writes `.old-monolith-done`.

  Log: `/tmp/monolith-old-<hash12>-<n>.log`.
- **Output.** stdout gets exactly one line on success, the binary's absolute path. Progress and errors go to stderr (`old-monolith: ...`); on failure: `FAILED`, the log, its last 20 lines, and always `left behind:`.
- **It never deletes anything** (no `rm`, no `git worktree remove` / `prune`); it lists what it leaves.

## The delete stand-in

Called as `rm`, `rmdir` or `unlink` (through the shim's symlinks), the program only logs `<name> shim (not deleting): <arguments>` on stderr (which is the build log) and exits 0. This is `DeleteStandIn`, checked before anything else.

## Using an old binary

Old binaries may lack the agent-shell window guard: run them only with `--dump-boxes`, `--dump-scene`, `--dump-scene-diff` or `--shot`. Build one old commit at a time (each is a full R3D build).

## Environment

| Variable | Use |
|---|---|
| `PATH` | The shims go in front of it for the build. |
| `R3D_DIR` | Removed for the build. |

No variables of its own.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | Built or reused. |
| 1 | Usage. |
| 2 | No git history, or the commit was not found. |
| 3 | Worktree or submodule setup failed. |
| 4 | The build failed (non-zero exit, or no binary). |

As `rm` / `rmdir` / `unlink`: always 0.

## Called by

The `migrate-map` skill (and the developer). Built by `./build.sh --tools`.
