# SQLite (vendored)

The SQLite amalgamation, the database of the model catalog `library/library.db`
(`tools/library.c`, `docs/model-library.md`). Only `build/tools/library` and its unit test
link it: the monolith build compiles `src/*.c` and `runtime/*.c`, which doesn't reach
`src/vendor/`, so monolith, the runtime, exports and the examples don't contain it.

| | |
|---|---|
| Source | https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip (https://www.sqlite.org/download.html) |
| Version | 3.54.1 (`SQLITE_SOURCE_ID` "2026-10-16 14:45:20 555e07ef33f69a0957d24d8e555b702464a131d6f1607f4c3c1e4551e8e6ca76"; the latest release on 2026-11-20) |
| Archive | sha256 `0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9`; SHA3-256 `92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30`, the value sqlite.org publishes for it |
| Files | `sqlite3.c`, `sqlite3.h` and `sqlite3ext.h` from the archive, byte for byte (its `shell.c` is left out) |
| Licence | Public domain (the notice at the top of each file: "The author disclaims copyright to this source code") |
| Downloaded | 2026-11-20 |
| Changes | none |

sha256 of the files as downloaded:

| File | sha256 |
|---|---|
| `sqlite3.c` | `0c657666d819f261cc4007526ad57e1669de9949df7a8ea847aa284a2bf0ee52` |
| `sqlite3.h` | `d6c84557261893439b341d2e6c35187ae3ea24fbfd843ad94616f72d89242b8c` |
| `sqlite3ext.h` | `48963abfefb41ac329ef9d706acb5eadba952a99a748285c9ded6b5e0942721b` |

How it is built (`nob.c`, function `LibraryVendor`, run by `./build.sh --tools library` and
`./build.sh test library`): C (`-std=gnu11`), `-w` (its warnings are not ours), `-O2 -DNDEBUG`,
with sqlite-vec in the same group of objects (`build/tools-obj/library/sqlite/`, one `.flags`
stamp, compiled only when stale) and these options:

- `-DSQLITE_ENABLE_FTS5`: the keyword index `models_fts`;
- `-DSQLITE_OMIT_LOAD_EXTENSION`: no extensions loaded from files (no `dlopen`); sqlite-vec is
  linked in instead;
- `-DSQLITE_DQS=0`: a double-quoted word is never taken for a string literal;
- `-DSQLITE_CORE`, `-DSQLITE_VEC_STATIC`, `-DSQLITE_VEC_OMIT_FS`: for sqlite-vec (its README).

The objects are packed into `build/tools-obj/library/libvendor.a` with the other vendored code.

To update: download the new amalgamation zip into an empty folder outside the repo, check its
SHA3-256 against sqlite.org's download page, read the change log, copy the three files here,
update this table and `docs/architecture.md` §2, then run `./build.sh test library` and the
leak check.
