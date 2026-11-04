# xatlas (vendored)

Mesh parameterization and chart packing by Jonathan Young, used by the editor's paint unwrap
(`src/paintunwrap.c`, `docs/painting.md` §4.5). Editor build only: the exported runtime and the
examples don't contain it.

| | |
|---|---|
| Source | https://github.com/jpcy/xatlas |
| Commit | `f700c7790aaa030e794b52ba7791a05c085faf0c` (2022-07-25, "Merge pull request #107 from T-rvw/upgrade_premake_to_use_vs2022"; the head of `master` on 2026-11-04) |
| Files | `source/xatlas/xatlas.cpp`, `source/xatlas/xatlas.h`, `source/xatlas/xatlas_c.h` and `LICENSE` at that commit, byte for byte (raw.githubusercontent.com/jpcy/xatlas/<commit>/...) |
| Licence | MIT (`LICENSE`; the same notice heads each source file, with thekla_atlas's and Fast-BVH's MIT notices) |
| Downloaded | 2026-11-04 |
| Changes | none |

sha256 of the files as downloaded:

| File | sha256 |
|---|---|
| `xatlas.cpp` | `0ed0283aad005c94738cb0cc4612dba264379d29dea5b3c9b242f2d4752d5df4` |
| `xatlas.h` | `e7675335ad8ab1c1cc9060ad153cf6b8ba2ee914282044eb5f02c49590218fbd` |
| `xatlas_c.h` | `3aef0438ca395a7c4c215de9fe40460a4530d19654bd21a3f849c76775fbc972` |
| `LICENSE` | `2c16d5b1c2808277fe975b4f70f0fd9afc9b3bcf04e6c676665a82cb2d5579e3` |

How it is built (`build.sh`, function `Xatlas`): as C++14 (C++11 fails on its single-threaded
path: `destroyGroup({ i })` on a struct with a default member initializer), with `-w` (its own
warnings are not ours; our code keeps zero), `-DNDEBUG` (no debug checks; its debug file writers
are compiled out by their own `#define ... 0` switches), `-DXATLAS_C_API=1` (the C API of
`xatlas_c.h`, which `paintunwrap.c` calls) and `-DXA_MULTITHREADED=0` (no worker threads: the same
model always gives the same unwrap), into one object per build folder (`build/<folder>-obj/xatlas.o`,
MinGW `build/win-obj/xatlas.o`). Sanitizer builds compile it with the same sanitizer; UBSan's
`-fsanitize=function` is turned off for it (its OpenNL part calls matrix destructors through a
pointer of the base struct type).

To update: download the four files at a new commit into an empty folder outside the repo, read
them, copy them here, update this table and run `tests/unit/paintunwrap_test.sh`.
