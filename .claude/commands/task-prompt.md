---
description: Draft a task prompt for a round of larger monolith engine or feature work
argument-hint: <what the developer wants>
---

Draft a task prompt for this round of monolith engine or feature work: $ARGUMENTS

Read `CLAUDE.md` and the `large-engine-work` skill (`.claude/skills/large-engine-work/SKILL.md`) first, and the sources the goal touches, so the prompt names real files and flags. Show the draft to the developer; don't start the work.

**This session only reads and drafts: it never writes or changes anything in the repo** — no code, shaders, build files, maps, skills, docs or TODO lists, not even a "small fix" spotted while reading, and no builds, `--apply-edits` or other commands that write into the repo. The only file it writes is the draft below. If the developer asks for a change during the session, say it belongs in the round the prompt starts (or a new session) and add it to the draft instead.

**Always store the draft in a file** as well: write it (Markdown, exactly the text shown) to `/tmp/monolith-task-prompts/<YYYY-MM-DD>_<short-slug>.md` (create the folder when missing; a revised draft of the same task updates that same file in place, never a new `…-v2.md`; never delete one) and give the developer the full path. The file is a hand-off copy outside the repo, not an instruction file.

A task prompt for a new round only needs what is new; the `large-engine-work` skill, the hard rules (`CLAUDE.md`), the other skills and the documentation (`docs/`) are already the agent's knowledge. A good task prompt has:

1. **Goal**: what the developer wants, as numbered outcomes, and why.
2. **Scope**: what may change (files, R3D, a map's look: normally not) and what must not.
3. **Phases** with their own "verify" step, in dependency order; the interfaces the phases share.
4. **Agents** (if wanted): the suggested split and which streams must be sequential.
5. **Done**: the deliverables and the evidence that proves each (shots, dumps, benches, leak check).
6. **The developer's calls**: which choices to make with a default and report, and which to ask about.

**Plain words, no jargon:** headings and steps say what happens ("Migrate the repo's maps with the skill"), not a slang name for it ("Dogfood", "smoke test", "wire up", "happy path"). Use the project's own terms (map package, bundle, `--shot`) and say the rest literally.

When a round is finished, move what it taught into the matching skill (and `docs/` where people need it) and its reasons into the `project-history` skill; don't keep the task prompt as an instruction file.
