# nob.h (vendored)

nob.h by Alexey Kutepov (<https://github.com/tsoding/nob.h>), the single-header library the build
program `nob.c` (repo root) is written with: it runs the build's commands (`Nob_Cmd`), echoes them,
and rebuilds `build/nob` when `nob.c` or this header changed (`NOB_GO_REBUILD_URSELF`). Build tool
only: nothing in the game, the runtime, an export or the examples contains it.

- Version: 3.10.1 (2026-10-28), commit `746f47daa5776a4c93fade3ead8c82cd469670ec` (the release has no
  git tag). sha256 of `nob.h`: `4ec35b6f463fd2041eafa484cbdcb17ae1ee2259abc5db1513122367fe41dc2a`.
- **Unmodified.** Anything monolith needs on top (timing, the redirects, the file operations) lives in
  `nob.c`. To update: replace `nob.h` with a newer release, update the lines above, rebuild
  (`./build.sh` recompiles `build/nob`), and check that every mode's command lines stay the same.
- Licence: the file offers MIT or public domain (Unlicense), the user's choice (its licence text is
  at the end of `nob.h`, kept as it is). monolith uses it under the MIT alternative, like the other
  vendored code (`src/vendor/xatlas/`).
