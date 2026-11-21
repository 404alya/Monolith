# `tools/stub-classifier.c`: a fake AI classifier

`tools/stub-classifier.c` is a fake AI classifier for tests of the editor's model choice (`src/classifier.c`; [request-queue.md](../request-queue.md), "Models"). It is built by `./build.sh --tools stub-classifier` into `build/tools/stub-classifier`. Overview of all the tools: [architecture.md §8](../architecture.md#8-tools).

## How it is used

Set `MONOLITH_CLASSIFIER=build/tools/stub-classifier`. The editor starts it exactly as it starts `claude -p` (the same arguments, the request on stdin, stdout into the bundle's `classifier.json`), and it answers the way Claude Code's `--output-format json` does, so no test ever starts a real Claude run.

## Environment

Each variable is optional.

### `MONOLITH_STUB_CLASSIFIER=<mode>`

What to answer (default: `default`):

| Mode | Answer |
|---|---|
| `fast` \| `default` \| `heavy` | A valid answer on that tier, from the `--model` it was given (`modelUsage` names it). The reason has a line break and a non-ASCII letter, which the editor must turn into plain ASCII. |
| `fallback` | A valid heavy answer from the `--fallback-model` (as if Fable was overloaded). |
| `garbage` | Text that is not JSON; exit 0. |
| `outside` | Valid JSON whose tier is `opus` (outside the schema); exit 0. |
| `fail` | A message on stderr; exit 1. |
| `slow` | Sleeps 600 s before answering (the editor's timeout must kill it). |
| `limit` | Claude Code's result for a run at the usage limit: `is_error: true`, `result` = `You've hit your session limit · resets 3:45pm`; exit 1. |

### `MONOLITH_STUB_LIMITED=<label>[,<label>…]`

Answers `limit` whatever the mode when its `MONOLITH_ACCOUNT` (`main` when unset) is in the list: the accounts that have "reached their usage limit" ([request-queue.md](../request-queue.md), "Usage limit and the fallback account").

### `MONOLITH_STUB_CLASSIFIER_LOG=<file>`, else `MONOLITH_STUB_LOG=<file>`

Appends what it was given to `<file>`: a line `stub-classifier account: <MONOLITH_ACCOUNT or -> config: <CLAUDE_CONFIG_DIR or ->`, a line `stub-classifier argv:`, one argument per line (each prefixed with `  arg: `), then `stub-classifier stdin:` and the input as it came.

It appends, so a stub terminal that replaces `MONOLITH_STUB_LOG` later in the same test doesn't hide it: give the classifier its own file with `MONOLITH_STUB_CLASSIFIER_LOG` when both run.

## Exit status

| Code | When |
|---|---|
| 0 | Answers, `garbage`, `outside`, `slow` when it is not killed. |
| 1 | `fail`, `limit` (also through `MONOLITH_STUB_LIMITED`), or an unknown mode. |

## Called by

The editor (`src/classifier.c`) in launcher tests and `build/tools/leakcheck`. (`build/tools/test-coordinator` sets `MONOLITH_CLASSIFIER=off`, so it never starts it.)
