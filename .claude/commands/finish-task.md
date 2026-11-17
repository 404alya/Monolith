---
description: Clean up after a task — leftover git worktrees, merged branches (local and on GitHub), stashes — without losing work
argument-hint: [anything to keep, e.g. a branch name]
---

Clean up what earlier tasks left behind in this repo. Keep these: $ARGUMENTS

The goal is a repo with only `main` (plus anything named above or still in use), and no work lost. Look before you remove anything; never remove the current checkout, `main`, or the branch checked out in it.

## 1. Look

Run these and keep the output for the report:

- `git fetch --prune` (drops remote-tracking refs whose branch is already gone on GitHub)
- `git worktree list`, then for every worktree other than the main checkout: its branch, `git -C <worktree> status --short --untracked-files=all` (changes under `.slop/` don't count as work) and `git log --oneline main..<branch>`
- `git branch -vv`, `git branch --merged main`, `git branch -r`, `git stash list`

## 2. Sort each item

- **Done:** a worktree with no uncommitted changes whose branch has no commits missing from `main`; a branch merged into `main`. A branch that was squash- or rebase-merged counts as merged when `git cherry main <branch>` lists only `-` lines or `git diff main...<branch>` is empty.
- **Has work:** uncommitted changes, or commits that `main` lacks. Before calling it lost work, check whether `main` already has it: compare the worktree's changed and untracked files with `main`'s (`cmp`, `git diff --no-index`), and look for a commit on `main` that did the same thing (`git log --oneline -20 main`). Agents' worktrees are often stale drafts of work that has since landed.

## 3. Ask once

Use AskUserQuestion with one question listing what you'd remove, grouped as:

- **Local, done:** worktrees and branches you'd remove without further ado.
- **On GitHub:** merged remote branches you'd delete (`git push origin --delete`). This changes the shared repo, so it always needs the developer's yes.
- **Has work:** each one with a one-line summary of what it changed and whether `main` already has it.

Offer "remove the done ones and the GitHub ones", "also remove the ones with work", and "show me the diffs first". When there's nothing to remove, say so and stop.

## 4. Remove what the developer approved

- **A worktree inside the repo** (`.claude/worktrees/…`): `gio trash <worktree folder>`, then `git worktree prune`. Never `rm`, and never `git worktree remove` (it deletes instead of trashing).
- **A worktree under `/tmp`**: `git worktree remove --force <path>` (`gio trash` can't trash there). Remove only one whose work is approved for removal.
- **A worktree folder that no longer exists:** `git worktree prune`.
- **A local branch:** `git branch -d <branch>`. Use `-D` only for a branch the developer approved under "has work", or a squash-merged one from step 2.
- **A remote branch:** `git push origin --delete <branch>…`.
- **A stash:** never drop one. List it in the report.

Leave the empty `.claude/worktrees/` folder, untracked files in the main checkout and everything else alone. Commit nothing.

## 5. Report

End with `git branch`, `git branch -r` and `git worktree list`, and say in a few lines what was removed, what was kept and why, and that trashed folders can be restored from the trash.
