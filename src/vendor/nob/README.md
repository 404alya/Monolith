# nob.h (vendored)

nob.h by Alexey Kutepov (<https://github.com/tsoding/nob.h>), the single-header library the build
program `nob.c` (repo root) is written with: it runs the build's commands (`Nob_Cmd`), echoes them,
and rebuilds `build/nob` when `nob.c` or this header changed (`NOB_GO_REBUILD_URSELF`). Build tool
for the build program, the repo's tools (`tools/*.c`, through `tools/tool.h`) and the examples' build; every export ships it (`nob.h`, `nob.c`, licence `LICENSES/nob.txt`) to build the viewer. Nothing in the game, the runtime or the examples' code contains it.

- Version: 3.10.1 (2026-10-28), commit `746f47daa5776a4c93fade3ead8c82cd469670ec` (the release has no
  git tag). sha256 of `nob.h` as it is here (modified): `127fa9ec5026b96c013bda169d60c619be48fcb195fe2bfff705fbb3247d5e88`;
  of the release's file: `4ec35b6f463fd2041eafa484cbdcb17ae1ee2259abc5db1513122367fe41dc2a`.
- **Modified: names only.** Every `va_list` variable and parameter is named `arg_ptr` (upstream: `args`,
  `ap`), and the copy in `nob_temp_vsprintf` is `arg_ptr_copy`, as in monolith's own code. Nothing else
  differs from the release. Anything monolith needs on top (timing, the redirects, the file operations)
  lives in `nob.c`. To update: replace `nob.h` with a newer release, repeat the rename (every `va_list`
  to `arg_ptr`, the copy to `arg_ptr_copy`), update the lines above, rebuild (`./build.sh` recompiles
  `build/nob`), and check that every mode's command lines stay the same.
- Licence: the file offers MIT or public domain (Unlicense), the user's choice (its licence text is
  at the end of `nob.h`, kept as it is). monolith uses it under the MIT alternative, like the other
  vendored code (`src/vendor/xatlas/`).
