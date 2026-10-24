# docs/knowledge: reference read on demand

`CLAUDE.md` is loaded into every session and holds only what every session needs: what the project is, the guard rails, the hard rules and the index of skills. **Workflows** (what to do, in which order, how to verify) are project skills in `.claude/skills/<name>/SKILL.md`; **reference** (formats, flags, architecture, engine facts) is here, linked from the skills, the code comments, the maps' `MAP.md` and `submodules/r3d/patches.md`.

| File | Contents | Used by |
|---|---|---|
| `map-format.md` | package layout, the `MAP.md` brief, the `.map` format: boxes, prefixes, groups, materials, environment, lights, viewpoints, models, limits | `map-request`, `map-editing`, `import-request`, `engine-code` (loader changes) |
| `request-bundle.md` | the files of a request bundle and every field of `request.md` / `request.json`, format-1 bundles | `map-request`, `engine-code` (bundle changes) |
| `engine.md` | technology, layout, data paths, code architecture, the scene runtime and its API, export / import / examples, known limitations | `engine-code`, `large-engine-work` |
| `cli.md` | controls, every command-line flag, environment variables, graphics settings | every skill that runs the game |
| `r3d-pitfalls.md` | R3D facts found by debugging: fog, shadows, caches, gotchas, ambient, view-independence | `lighting-fog-shadows`, `engine-code` |
| `export.md` | `--export` / F9: the exported folder, prebuilt libraries, how to verify an export | `engine-code`, `import-request` |
| `history.md` | the development rounds and the reasons behind the current shape | anyone asking why |

Skills: `map-request`, `import-request`, `map-editing`, `assets-and-licences`, `engine-code`, `lighting-fog-shadows`, `benchmarking`, `leak-check`, `large-engine-work`, `coordinator` (user-invoked only); slash command `/task-prompt` (`.claude/commands/`).

Other docs: `docs/performance.md` (measurements), `docs/design-v2.md` (design record of round 2), `docs/design-coordinator.md` (the request queue and coordinator), `docs/QA-REPORT.md` (QA pass), each map's `MAP.md` (its style and rules, and the extra views for engine verification), `submodules/r3d/patches.md` (R3D changes).

**Keeping it tidy:** one topic per file or skill, no duplicated reference text (link instead), measurement history in `docs/performance.md`. Skills and `CLAUDE.md` never name a map; per-map rules go into that map's `MAP.md`. When a task prompt's round is finished, move what it taught into the matching skill or file here (reasons into `history.md`) instead of keeping the prompt as an instruction file. The game's bundles name `CLAUDE.md` and the skill files `.claude/skills/map-request/SKILL.md` and `.claude/skills/import-request/SKILL.md`, and the coordinator's start prompt names `.claude/skills/coordinator/SKILL.md` (`src/launcher.c`, `src/import.c`): keep those paths, or change the code with them.
