# sqlite-vec (vendored)

Vector search for SQLite by Alex Garcia: the `vec0` tables `vec_shape`, `vec_style` and
`vec_use` of the model catalog and `vec_distance_cosine` / `vec_int8`, which `library search`
uses for its exact full scan (`tools/library.c`, `docs/model-library.md` §3, §6). Only
`build/tools/library` and its unit test link it.

| | |
|---|---|
| Source | https://github.com/asg017/sqlite-vec |
| Release | `v0.1.9` (2026-03-31; the latest release on 2026-11-20), commit `e9f598abfa0c06b328d8fe5da9c3760cce74be10` (also `SQLITE_VEC_SOURCE` in `sqlite-vec.h`) |
| Archive | https://github.com/asg017/sqlite-vec/releases/download/v0.1.9/sqlite-vec-0.1.9-amalgamation.tar.gz, sha256 `3acd67cb4aff080c7050926fd3cf8227905fe5b7ee3829d8ee5024ab1283cf61` |
| Files | `sqlite-vec.c` and `sqlite-vec.h` from the archive, byte for byte; `LICENSE-MIT` and `LICENSE-APACHE` from the tag (raw.githubusercontent.com/asg017/sqlite-vec/v0.1.9/…), since the amalgamation carries no licence text |
| Licence | MIT or Apache 2.0, at the user's choice (`LICENSE-MIT`, `LICENSE-APACHE`); used as MIT |
| Downloaded | 2026-11-20 |
| Changes | none |

sha256 of the files as downloaded:

| File | sha256 |
|---|---|
| `sqlite-vec.c` | `ba081a47fa02eadc3cf6b16c314b695b84081269349aac722b4efa338fe8fd85` |
| `sqlite-vec.h` | `8e4d7bfcd779c89bd19a6b2959fce24ee391b2eaf79a85a979377a36627cb060` |
| `LICENSE-MIT` | `6ce72bbe12d975bd5286e5ab0a064c069693300c47bccbc57bec18485f1621ea` |
| `LICENSE-APACHE` | `a38070a94d4afd9cd710e3ce67bd1de78097cfe1784c1f0109ac95d3c196bfdc` |

How it is built (`nob.c`, function `LibraryVendor`): in the same group as the SQLite
amalgamation (`build/tools-obj/library/sqlite/`), C, `-w -O2 -DNDEBUG`, with
`-DSQLITE_CORE` (compiled as part of SQLite, not as a loadable extension: it includes
`sqlite3.h`), `-DSQLITE_VEC_STATIC` (no export attribute) and `-DSQLITE_VEC_OMIT_FS` (no SQL
functions that read files). Its SIMD paths (`SQLITE_VEC_ENABLE_AVX` / `_NEON`) stay off: the
int8 cosine distance is the plain loop, the same on every machine. `tools/library.c` registers
it on every connection with `sqlite3_vec_init(db, &err, NULL)` (it defines `SQLITE_CORE` before
including `sqlite-vec.h`).

To update: download the amalgamation of a new release into an empty folder outside the repo,
read it, copy the two files (and the licences from the same tag) here, update this table and
`docs/architecture.md` §2 (a new database records the version from `SQLITE_VEC_VERSION` in
`meta.sqlite_vec`), then run `./build.sh test library` and the leak check.
