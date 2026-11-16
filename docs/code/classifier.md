# `src/classifier.c`: the AI classifier

One `claude -p` run per request that picks its tier ([request-queue.md](../request-queue.md), "Models"). The launcher decides when to run it (`launcher.c` region "Model choice", [launcher.md](launcher.md)).

Linux only, like the launcher (`waitpid`, `kill`): left out of the view-only build (`nob.c`).

## The run

The run is a plain child process started through nob.h (`src/vendor/nob/nob.h`, vendored unmodified; this is the only monolith source that implements it): a `Nob_Cmd` built with `nob_cmd_append` (an argument list: no shell, nothing quoted by hand) and `nob_cmd_run(.async, .stdin_path, .stdout_path, .stderr_path)`, so the call returns at once.

The request text is data: it goes to the child's stdin (`classifier-input.txt`), never on its command line.

## Polling

`waitpid(pid, &status, WNOHANG)` on the POSIX `Nob_Proc` (a pid), once per call, never sleeping. Not nob's `nob__proc_wait_async(proc, 0)`: it doesn't sleep either, but it only says done / running / failed and logs every non-zero exit as an error, and the failure reason here wants the exit status ("exit 1", "killed by signal 9"), as `tools/tool.h` keeps it.

nob.h has no kill: a timeout sends SIGTERM, then SIGKILL `KILL_GRACE` seconds later; the next poll reaps it.

## The answer

`classifier.json` is Claude Code's `--output-format json` result object. Only `is_error`, `structured_output {tier, reason}` and `modelUsage` (which model answered) are read, by the small JSON reader in the file. `tier` must be exactly `"fast"`, `"default"` or `"heavy"`; anything else is a failure (the launcher then uses the default tier). The reason is cut to plain ASCII.
