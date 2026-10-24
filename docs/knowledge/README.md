# docs/knowledge: what the AI reads, when

`CLAUDE.md` is loaded into every session and holds only what every session needs (what the project is, the guard rails, the hard rules). Everything else is read on demand:

| File | Read it when | Contents |
|---|---|---|
| `AI-MAP-TOOL.md` (repo root) | you were started for a request (`requests/<id>/request.md`), usually as the coordinator's subagent | the request workflow: bundle files, working under the coordinator, locating the target, coordinates, editing rules, verification, `result.md` |
| `AI-COORDINATOR.md` (repo root) | you are the request coordinator (the game started you with that prompt) | the queue, the loop, the scheduling rules, questions and cancels, recovery, the subagent prompt |
| `map-format.md` | you edit or create a map, or change the loader | package layout, the `MAP.md` brief, the `.map` format: boxes, prefixes, groups, materials, environment, lights, viewpoints, models, limits |
| `models-and-textures.md` | a map needs a model or a texture | sources and licences, safe download, `--inspect-model`, normalising, recording (`ASSETS.md`, sidecars), restyling |
| `import-export.md` | an import request, or export questions | import procedure, the capture shim, `--export` |
| `PROMPT-INSTRUCTIONS.md` (repo root) | you change code: C, R3D, build, runtime | technology, layout, build, flags, architecture, **R3D pitfalls (§7)**, verification, conventions |
| `working-method.md` | larger engine or feature work, multi-agent rounds, writing a task prompt | decisions, multi-agent method, performance method, reporting |
| `history.md` | a rule or design looks arbitrary | the two development rounds and the reasons behind the current shape |

Other docs: `docs/performance.md` (measurements), `docs/design-v2.md` (design record of round 2), `docs/design-coordinator.md` (the request queue and coordinator), `docs/QA-REPORT.md` (QA pass), each map's `MAP.md` (its style and rules), `submodules/r3d/patches.md` (R3D changes).

**Keeping it tidy:** one topic per file, no duplicated reference text (link instead), measurement history in `docs/performance.md`. When a task prompt's round is finished, move what it taught here (rules into the matching file, reasons into `history.md`) instead of keeping the prompt as an instruction file. The game's bundles point at `CLAUDE.md` and `AI-MAP-TOOL.md` and at its sections "Import requests" and "Models and textures"; keep those names.
