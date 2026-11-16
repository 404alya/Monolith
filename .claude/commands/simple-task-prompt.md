---
description: Draft a short task prompt for a simple monolith task that needs no verification
argument-hint: <what the developer wants>
---

Draft a short task prompt for this simple monolith task: $ARGUMENTS

Read `CLAUDE.md` and the sources the task touches first, so the prompt names real files, functions and flags. Show the draft to the developer; don't start the work.

**This session only reads and drafts: it never writes or changes anything in the repo** — no code, shaders, build files, maps, skills, docs or TODO lists, not even a "small fix" spotted while reading, and no builds, `--apply-edits` or other commands that write into the repo. The only file it writes is the draft below. If the developer asks for a change during the session, add it to the draft instead.

**Always store the draft in a file** as well: write it (Markdown, exactly the text shown) to `/tmp/monolith-task-prompts/<YYYY-MM-DD>_<short-slug>.md` (create the folder when missing; a revised draft of the same task updates that same file in place, never a new `…-v2.md`; never delete one) and give the developer the full path. The file is a hand-off copy outside the repo, not an instruction file.

**A simple task** is one session's work in one place: one agent, no phases, no shared interfaces, no change to how a map looks, no change under `submodules/r3d`, no performance claim. If reading the sources shows the task is larger than that, say which of these it breaks and that `/task-prompt` is the command for it; draft nothing until the developer decides.

The prompt is short (about half a page) and has only what is new; `CLAUDE.md`, the skills and `docs/` are already the agent's knowledge, so don't repeat them. It has:

1. **Task**: what the developer wants and why, in two or three sentences.
2. **Where**: the files and functions to change, and anything nearby that must stay as it is.
3. **Done**: the result, in a sentence or two. No check to run.

**No verification:** these tasks are small, so the prompt asks for none. No check to run, no phases with "verify" steps, no evidence list, no `--shot`, `--dump-boxes` or other comparison of any map, no benchmarks, no verification by a second agent, no final summary document. `CLAUDE.md` waives all verification for a simple task: no proof that every map is unchanged, no builds (not even to check warnings or that it compiles), no leak check, no tests. The prompt doesn't ask for any of them. Start the prompt with the line "This is a simple task (`/simple-task-prompt`)." so the agent knows the waiver applies.

Leave out the developer's calls unless there is a real choice: then name it in one line with the default to take.

**Plain words, no jargon:** say what happens ("Rename the flag in `src/cli.c`"), not a slang name for it ("wire up", "smoke test", "happy path"). Use the project's own terms (map package, bundle, `--dump-boxes`) and say the rest literally.
