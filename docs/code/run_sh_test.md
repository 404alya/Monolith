# `tests/unit/run_sh_test.c`: the test of run-sh

The test of `tools/run-sh.c` (`build/run-sh.o`; [run-sh.md](run-sh.md)). `./build.sh test run_sh` ([nob.md](nob.md), region "Unit tests") builds `run-sh.c` twice into `build/unit/run_sh/` (`run-sh.o` with the flags of `./build.sh --tools`, `run-sh-san.o` with ASan + UBSan) and calls

```
build/unit/run_sh/run_sh_test <run-sh.o> <run-sh-san.o> <out folder> <repo root>
```

## What it checks

1. **The rules table.** Every command line in `CASES` goes through `run-sh.o --check` (which never runs them), with the repo root as the working directory and with both builds; the decision (allowed / refused, and the rule's name in the refusal) must be the expected one. The table covers each rule's typical refusals and allowed cases, and the commands sessions run every day (the must-pass list). It has no rows for evasions (nested shells, interpreters, more wrappers): run-sh.o guards against ordinary mistakes and doesn't look for them ([run-sh.md](run-sh.md#what-it-doesnt-see)). The sanitizer build's runs fail on any leak, UBSan or ASan report (LeakSanitizer's exit code differs from 0 / 126), so "zero leaks" covers run-sh.o itself. `<scratch>` in a case is a folder under `/tmp/claude-<uid>/` (a session scratchpad's place), which holds a real symlink to the repo (`link-to-repo`) for the ways out of `/tmp`.
2. **`-c` passes everything through:** `-c 'exit 7'` exits 7, stdin reaches the command, the arguments after the command are `$0 $1`, a refused command exits 126 with the refusal on stderr and runs nothing, and `-c 'sleep 30'` stops on SIGTERM and on SIGINT.
3. **The benchmark lock:** a `--bench` run (a fake monolith: `<scratch>/fakebin/monolith`, a link to `true`) holds the lock until its shell ends; a second one at the same time is refused, and runs once the first has ended.
4. **The log:** every decision above is in `<out>/state/log` (`RUN_SH_DIR` points run-sh.o there, so the real `build/run-sh/log` is untouched).
5. **The hook:** JSON on stdin, the exact answer on stdout (plain, already wrapped, `run_in_background` and `timeout` kept, single quotes, JSON escapes, Monitor, a WebSocket Monitor, a refused command, the permission modes that only check, `--rewrite all`, input that isn't JSON).

Exit 0 when everything passed, 1 otherwise (each failure printed with what came back).

## The scratch folder

`/tmp/claude-<uid>/run-sh-test/` is reused between runs and never deleted (nothing in this repo deletes with `rm`; it is under `/tmp`, which the system empties). A `link-to-repo` left there by a run from another checkout (a worktree) is made to point at this repo again before the cases run.
