# The model library

This folder holds monolith's **model catalog**: a list of free 3D models from Poly Haven,
Kenney and Polyfork that a map can use, searched by what a model looks like and what it is for.
The catalog stores links, licences, file hashes, sizes and our own short descriptions. It
stores no model files: a model is downloaded only when a map uses it, and the download is
checked against the hash recorded here.

| File | What it is |
|---|---|
| `library.db` | the catalog, a SQLite database (stored with Git LFS: run `git lfs install` once per machine) |
| `library.dump.txt` | the same catalog as plain text, committed with every change so a change can be read in a diff |
| `EMBEDDING.md` | the language model that turns descriptions into vectors (about 600 MB, kept outside the repo) |

The design and its reasons are in [docs/model-library.md](../docs/model-library.md); every
command and flag is in [docs/code/library.md](../docs/code/library.md).

## Licences

Every row is either **CC0 1.0** (Poly Haven, Kenney: no conditions) or **CC BY 4.0** with its
credit line (Polyfork: the credit must be kept). Nothing else is ever indexed. A model copied
into a map package still gets its `ASSETS.md` row and sidecar, like any other asset.

## Searching it

Build the tool once (`./build.sh --tools library`) and put the embedding model in place
(`EMBEDDING.md`). Then describe what you need in three parts, each optional:

```
build/tools/library search --shape "tall narrow conifer, conical crown" \
                           --style "faceted low poly" \
                           --use "forest edge, background tree line" \
                           --max-tris 2000 --cc0-only
```

- `--shape`: the silhouette and parts; `--style`: how the geometry is made (facets, bevels,
  proportions); `--use`: where it goes in a level. `--words` adds plain keywords.
- Filters: `--style-tag lowpoly|stylised|realistic|photoscan|voxel|toon`, `--source
  polyhaven|kenney|polyfork`, `--max-tris N`, `--max-size M` (the largest side, in the file's
  units), `--cc0-only`, `--limit N`.

Each result line gives the id, a score, `best-cos` (how close the closest of your three texts
is: 1 is identical), triangles, size, category and shape description. A `?` means the source
didn't say. The same search on the same catalog always gives the same lines.

Descriptions talk about **geometry and use only**, never colour or material: a map's material
overrides replace those anyway, so search for the shape you need and restyle it.

## Getting a model

```
build/tools/library show kenney-nature-kit/tree_pineTallA
```

prints the whole row and, last, the exact download command (`python3 -I tools/fetch_asset.py
…`) with the hash it must match. A Polyfork model has no hash until its first download: after
it, record the file's sha256 with `build/tools/library pin <id> <sha256>`. Then inspect,
normalise and record the model as for any asset (the `assets-and-licences` skill).

## Adding a source or a pack

The catalog is rebuilt from text files, never edited by hand:

1. `python3 -I tools/fetch_asset.py <source> catalog --out /tmp/monolith-library` writes one
   JSON line per model with everything the source publishes (links, licence, hashes, sizes).
2. Descriptions are added to each line (`category`, `style`, `shape_desc`, `style_desc`,
   `use_desc`; the rules are in `docs/model-library.md` §4).
3. `build/tools/library ingest <file.jsonl>` checks every line and adds or updates the rows
   (nothing is written if any line is wrong); `build/tools/library embed` makes the vectors of
   the new or changed rows; `build/tools/library check` must print `OK`.
4. `build/tools/library dump > library/library.dump.txt`, and commit the database with its dump.
