# `tests/unit/export_strip_test.c`: no map-text parser in an export

An export ([export.md](../export.md)) carries `runtime/monolith_scene.c` and `.h` as a game has them: compiled maps only ([compiled-map.md](../compiled-map.md), "The two builds"). This test checks that those two files hold none of the editor's parts.

## The rules

Each rule is a search of the two files for names or text; any hit fails the test. There must be:

- no tokenizer or statement parser,
- no model-file reference scan,
- no compiler,
- no number parsing (`strtod` / `strtof` / `strtol` / `sscanf` …),
- no `.map` file lookup,
- no `MONOLITH_EDITOR` left.

Patterns are POSIX extended regular expressions (`<regex.h>`), as `grep -E` reads them. A rule marked "whole word" is `grep -w`: a match counts only when it is not part of a longer word (the characters next to it are not letters, digits or `_`). That is done by wrapping the pattern in a boundary check, which finds the same lines `grep -w` does (it tries every match, not only the first).

## Running it

It replaces `tests/unit/export_strip_test.sh` (a grep per rule). Built and run by `./build.sh test export_strip` ([nob.md](nob.md), `UNIT_TESTS`): plain C and libc, with the unit tests' sanitizers.

| Argument | Meaning |
|---|---|
| `argv[1]` | The repo root (passed by `nob.c`). |
| `argv[2]` | The test's own folder (`build/unit/export_strip`), where it makes its export (passed by `nob.c`). |
| `argv[3]` | Optional (after `--` on the command line): an existing export folder to check instead of making one. |

Without `argv[3]` it exports `tests/models` with `build/linux/monolith` into `<argv[2]>/export-<YYYYmmddHHMMSS>/` (monolith's `--export`, hidden: no window), checks that, and moves the export to the trash afterwards (`gio trash`, the repo's only way to delete). With `argv[3]` it checks that folder and leaves it alone.

## Output and exit codes

Line for line as the script printed it: `ok   no <rule>` per rule that passed; `FAIL <rule>:` and the hits (`path:line:text`, indented four spaces) per rule that didn't; then `export_strip_test: OK (<dir>)` or `export_strip_test: FAILED (<dir>)`.

Exit 0 when all rules passed; 1 on any hit, a missing file, a missing monolith binary or a failed export.
