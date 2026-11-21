# Model library: a local catalog searched by meaning

Status: design note for the round of 2026-11-20, approved by the developer on the same day. Implementation notes are added as the phases are merged.

A request that needs a model used to search Poly Haven, Kenney or Poly Pizza on the web, pick a result and download it. Now the search runs against **`library/library.db`**, a SQLite catalog that ships with the repo. It has one row per model from Poly Haven, Kenney, Polyfork and Poly Pizza, holding the model's links, licence, file hash, measured facts and our own descriptions, plus three embedding vectors and a keyword index. **No model file is stored in the repo**: Poly Haven and Polyfork and Poly Pizza files stay at their sources, and Kenney zips stay in a local cache on the developer's machine (§2). A model is downloaded only when a map uses it, by `tools/fetch_asset.py`, which checks the file against the hash in the catalog row.

Why: the web search was slow, unpredictable and repeated for every request. With a catalog, the same query against the same `library.db` always returns the same IDs. It can match on shape and use rather than a source's keywords, it can be reviewed in git, and the licence is checked once, when a model is indexed.

## 1. Parts

| Part | Where | Job |
|---|---|---|
| Catalog | `library/library.db` (Git LFS) | rows, vectors, keyword index, `meta` |
| Review dump | `library/library.dump.txt` (plain git) | `library dump` output, committed with each change to the database so changes show as text diffs (the database itself is in LFS and can't be diffed) |
| Catalog tool | `tools/library.c` → `build/tools/library` | ingest, embed, search, show, pin, check, dump, eval. No network access. The only program that links SQLite, sqlite-vec and llama.cpp |
| Harvester | `tools/fetch_asset.py` (`<source> catalog`) | reads the sources' APIs and pages and writes `.jsonl`. Network access stays in this one audited file |
| Downloader | `tools/fetch_asset.py` (`<source> get … --expect-*`) | downloads on use and refuses a hash mismatch |
| Embedding model | `~/.cache/monolith/embed/Qwen3-Embedding-0.6B-Q8_0.gguf`, recorded in `library/EMBEDDING.md` | 600 MB, outside the repo. `MONOLITH_EMBED_DIR` overrides the folder |
| People's guide | `library/README.md` | what the catalog is, how to search it, and how to add a source or a pack |
| Reference | `docs/code/library.md` | every command, flag, exit code and environment variable |

## 2. Sources and licences

| Source | Licence recorded | Hash at indexing | How it is indexed |
|---|---|---|---|
| Poly Haven | CC0 1.0, https://polyhaven.com/license | md5 of the 1k `.gltf` (API) | API only: `/assets?t=models`, `/info/<slug>`, `/files/<slug>` |
| Kenney | CC0 1.0 (`License.txt` in every zip) | sha256 of the zip entry, plus the zip's sha256 | every 3D pack with glTF/GLB models (the developer's choice), from the **local zip cache** `~/.cache/monolith/kenney/` (`MONOLITH_KENNEY_ZIPS` overrides it). The developer downloads the zips there in a browser. On 2026-11-20, kenney.nl's `robots.txt` disallowed Claude's agents site-wide and the pack pages gave only browser-signed zip links, so no agent fetches from kenney.nl. `kenney catalog` audits, lists and measures each cached zip and never moves or changes it |
| Poly Pizza | per model, as the API gives it: CC0 1.0, or CC BY 3.0 (https://creativecommons.org/licenses/by/3.0/) with the API's attribution text; any other licence is not indexed | none (the API publishes no hashes). `library pin` on first download | official API v1.1 only (`https://api.poly.pizza/v1.1`, header `x-auth-token`, the developer's key from `POLY_PIZZA_API_KEY` or `~/.config/monolith/poly-pizza.key`, never printed or stored), searches swept by category and paged, deduplicated by id; at least 2 s between requests. `polypizza get` checks the licence again at download time, as it always has |
| Polyfork | CC BY 4.0, https://creativecommons.org/licenses/by/4.0/ (the developer's choice), with the API's `credit` text as the attribution | none (the API publishes no hashes). `library pin` records one on first download | `GET /api/assets?free=1&per_page=100` (pages), then `/api/assets/{id}`. No file downloaded |

Polyfork terms (https://polyfork.dev/licensing, read 2026-11-20):
- CC BY asks for one credit line, e.g. `Fox by Polyfork (https://polyfork.dev/asset/fox-base), CC BY 4.0`, and a statement when a model was changed. `record_asset.py` already records every change to the file.
- "fetch a model when someone uses it rather than the catalogue in bulk, send a User-Agent that names your product, and back off when we answer 429". The harvester sends the existing user agent, waits at least 1 s between requests, and on HTTP 429 waits as `Retry-After` says (60 s when it is missing) with exponential backoff.
- Not under CC BY: the `.mjs` modules, the HD editions, renders and thumbnails, and "the text and data published beside the models". None of these is downloaded into the repo or stored anywhere. Renders are only *viewed* to write descriptions. The catalog keeps only facts that are needed to find and credit a model: the ID, the page URL, the title (it is part of the required credit), the class, the kit, triangles, size, material count, rig flags and the `credit` line. The developer accepted this (§11.1).

Poly Pizza's terms (https://poly.pizza/docs/tos, read 2026-11-20) forbid "data mining, or any form of extraction for machine learning or artificial intelligence … without explicit written permission". On 2026-11-20 the developer was shown this clause and decided to index Poly Pizza anyway, through the official API only.

The catalog indexes only CC0 rows and CC BY rows (4.0 for Polyfork, 3.0 or 4.0 for Poly Pizza) that have attribution text. `library ingest` refuses any other row. A model copied into a package still gets its `ASSETS.md` row and sidecar (steps 5–8 of the `assets-and-licences` skill).

## 3. Schema (schema version 1)

```sql
CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID;
-- keys: schema_version '1', embed_model 'Qwen3-Embedding-0.6B', embed_file 'Qwen3-Embedding-0.6B-Q8_0.gguf',
--       embed_sha256 '<64 hex>', embed_quant 'Q8_0', embed_pooling 'last', dims '512', vector_format 'int8',
--       quantisation 'maxabs-127' (see §5), instruct_shape / instruct_style / instruct_use '<the query instructions>',
--       llama_cpp '<release tag> <commit>', sqlite_vec '<version>'

CREATE TABLE models (
    rowid        INTEGER PRIMARY KEY,         -- stable within one database; the vec tables key on it
    id           TEXT NOT NULL UNIQUE,        -- polyhaven-<slug> | kenney-<pack>/<file stem> | polyfork-<asset id> | polypizza-<id>
    source       TEXT NOT NULL,               -- polyhaven | kenney | polyfork | polypizza
    name         TEXT NOT NULL,               -- the source's title (Polyfork: part of the credit)
    collection   TEXT,                        -- Kenney pack slug, Polyfork kit; NULL for Poly Haven
    page_url     TEXT NOT NULL,
    file_url     TEXT NOT NULL,               -- Poly Haven: the 1k .gltf; Kenney: the pack page (no stable zip URL); Polyfork: /cdn/<id>.glb
    path_in_zip  TEXT,                        -- Kenney only
    file_hash    TEXT,                        -- hex; NULL until pinned (Polyfork)
    hash_kind    TEXT,                        -- md5 | sha256 | NULL (with file_hash)
    zip_sha256   TEXT,                        -- Kenney only
    licence      TEXT NOT NULL,               -- 'CC0 1.0' | 'CC BY 3.0' | 'CC BY 4.0'
    licence_url  TEXT NOT NULL,
    attribution  TEXT,                        -- required for CC BY (Polyfork's credit, Poly Pizza's attribution)
    authors      TEXT,                        -- Poly Haven's authors, '; '-separated; Kenney 'Kenney'; Polyfork 'Polyfork'
    rigged       INTEGER,  ik INTEGER,        -- Polyfork's has_rig / has_ik; 0 for the others
    category     TEXT NOT NULL,               -- ours: one lower-case noun phrase ('tree', 'street lamp', 'crate')
    style        TEXT NOT NULL,               -- §4 vocabulary
    style_desc   TEXT NOT NULL, shape_desc TEXT NOT NULL, use_desc TEXT NOT NULL,
    tags         TEXT NOT NULL,               -- space-separated, lower case: source tags/categories, Polyfork class, path words
    tris         INTEGER,                     -- NULL where unknown
    sx REAL, sy REAL, sz REAL,                -- size in file units (glTF: metres; Kenney kits are tile units, §2 of the
                                              -- assets-and-licences skill), NULL where unknown
    slots        INTEGER,                     -- material slots
    flat_shaded  INTEGER, textured INTEGER, factor_colours_only INTEGER,   -- 0/1/NULL
    embed_hash   TEXT,                        -- sha256 of the three texts embedded (§5); NULL = vectors missing
    indexed_at   TEXT NOT NULL                -- YYYY-MM-DD the row was added or its content last changed
);
CREATE INDEX models_source ON models(source);

CREATE VIRTUAL TABLE models_fts USING fts5(category, tags, shape_desc, style_desc, use_desc, name,
                                           content='models', content_rowid='rowid', tokenize='unicode61');
CREATE VIRTUAL TABLE vec_shape USING vec0(embedding int8[512] distance_metric=cosine);
CREATE VIRTUAL TABLE vec_style USING vec0(embedding int8[512] distance_metric=cosine);
CREATE VIRTUAL TABLE vec_use   USING vec0(embedding int8[512] distance_metric=cosine);
```

`name`, `collection` and `authors` are additions to the brief's column list: search output and `show` need a name, the pack or kit is a useful filter, and `record_asset.py` needs the authors. `embed_hash` lets `embed --missing` find rows whose text changed.

Measured facts per source:

| Fact | Poly Haven | Kenney | Polyfork |
|---|---|---|---|
| `tris` | API `polycount` if given, else NULL | `--inspect-model` | API `triangles` |
| size | API `dimensions` (mm → m) if given, else NULL | `--inspect-model` | API `size_m` |
| `slots` | NULL | `--inspect-model` | API `materials` |
| `flat_shaded` | 0 | vertices ÷ triangles ≥ 2.5 (split vertices; glTF has no flat-shading flag and `--inspect-model` reports glTF materials as `pbr`) | 1 (the whole source) |
| `textured` | 1 | any map in `--inspect-model` | NULL |
| `factor_colours_only` | 0 | no albedo map in any slot | NULL |

## 4. Descriptions

They describe **geometry and use only**. A map's material overrides replace colour and texture anyway. No colour words, and no material or texture words (`wooden`, `rusty`, `mossy`, `brick`, `glossy` …). A word list in `tests/unit/library_test.c` checks every description.

**`style`** must be one of these (the default chosen for this round):

| Tag | Means |
|---|---|
| `lowpoly` | visible facets, few triangles, simple silhouettes (Kenney kits, Polyfork) |
| `stylised` | smooth but exaggerated or simplified proportions, bevelled forms |
| `realistic` | modelled to real proportions with fine detail |
| `photoscan` | scanned: irregular dense surface, real-world wear in the geometry (most Poly Haven rocks, logs, plants) |
| `voxel` | built from cubes |
| `toon` | cartoon proportions, chunky outlines in the geometry |

Templates (one to three sentences each, present tense, no brand or source names):
- **`shape_desc`**: the silhouette, proportions and parts. "Tall narrow conifer: straight trunk, conical crown of five stacked tiers, base wider than height/3." Rough size where it helps ("knee-high").
- **`style_desc`**: the geometry style, meaning faceting, edge treatment and exaggeration. "Hard facets, no bevels, every surface flat; proportions slightly chunky." It is written once per Kenney pack and once for Polyfork, and checked per class.
- **`use_desc`**: where it goes and what it is for. "Forest edges, mountain paths, background tree lines; a landmark at small scale."

## 5. Embeddings

- Model: **Qwen3-Embedding-0.6B**, GGUF **Q8_0** (`Qwen3-Embedding-0.6B-Q8_0.gguf`, 639 MB, Apache 2.0, sha256 `06507c7b42688469c4e7298b0a1e16deff06caf291cf0a5b278c308249c3e439` as listed on the model card; checked on download in phase 1). It runs on the vendored llama.cpp, CPU only.
- **Last-token pooling** (`LLAMA_POOLING_TYPE_LAST`). The tokenised input must end in `<|endoftext|>`. Phase 1 checks that the GGUF's tokenizer appends it, and appends it itself if not.
- **Cut to 512** (Matryoshka: the first 512 of 1024 dimensions), then L2-normalised, then quantised to int8 per vector: `q = round(127 · x / max|x|)`. Cosine distance ignores the per-vector scale, and this uses the whole int8 range, where a fixed `127 · x` would leave most values at a few steps. `meta.quantisation` records this. Queries are quantised the same way and compared with `vec_distance_cosine`.
- Texts embedded per model (no instruction): shape = `"<category>: <name>. <shape_desc>"`, style = `"<style>. <style_desc>"`, use = `"<category>. <use_desc>"`. `embed_hash` = sha256 of the three texts joined by `\n\0`.
- Query texts get the instruction prefix `Instruct: <task>\nQuery:<text>` (the model card's format). The tasks, stored in `meta`:
  - shape: `Given a description of an object's shape, retrieve 3D models with that shape`
  - style: `Given a description of a geometry style, retrieve 3D models modelled in that style`
  - use: `Given where and how an object is used in a level, retrieve 3D models that fit that use`
- **Determinism:** llama.cpp is compiled with fixed flags (x86-64-v3: AVX2 + FMA, no `-march=native`) and run with a fixed thread count (4) and batch size, so a query gives the same vector on every run on this machine. A unit test embeds the same text twice and compares the bytes. With `--embedder stub`, a vector comes from a SHA-256 stream of the text, which the tests use without the GGUF.
- `library` checks the GGUF's sha256 against `meta.embed_sha256` the first time a run needs the model (cached by size and mtime in `~/.cache/monolith/embed/<file>.sha256`). On a mismatch it refuses with exit 3.

## 6. Search

```
library search --shape TEXT --style TEXT --use TEXT [--words TEXT] [--style-tag T]... [--source S]...
               [--max-tris N] [--max-size M] [--cc0-only] [--limit N] [--weights s,t,u,w]
```
At least one of `--shape`, `--style`, `--use` or `--words` is required.

1. **Filter in SQL** on `models`: `style IN (…)`, `source IN (…)`, `tris <= N`, `max(sx,sy,sz) <= M`, `licence = 'CC0 1.0'`. Rows with NULL `tris` or size **pass** those filters and are flagged with `?` in the output.
2. **Per vector** that has query text: compute exact cosine distance over the filtered rows (`vec_distance_cosine` in a full scan. About 15 000 rows × 512 bytes is well under 10 ms, and a full scan gives exact, reproducible order, which an approximate index wouldn't), then keep the top 50, ordered by `distance, id`.
3. **Words:** FTS5 `bm25` over `models_fts`, top 50 ordered by `bm25, id`. The query is the words of `--words`, or, if that is absent, of `--shape`/`--use`, each quoted as an FTS5 string and joined by `OR`. Quoting means no FTS5 syntax from the request text can get through.
4. **RRF:** `score(id) = Σ_lists w_list / (60 + rank)`, where rank starts at 1. Weights default to `1,0.5,1,3` (shape, style, use, words; phase 4 measured them, §12; the round started with `1,1,1,1`). Lists without query text are left out.
5. Sort by `score DESC, id ASC` and print `--limit` (default 10) lines, tab-separated:
   `id  score(6 decimals)  best-cos(4 decimals)  tris|?  sx×sy×sz|?  category  shape_desc`
   `best-cos` is the highest cosine similarity among the vector lists. The "nothing fits" threshold is set on it in phase 4, because RRF scores are only ranks and don't say how close a match is.

## 7. Command line of `build/tools/library`

Global flags: `--db PATH` (default `<repo>/library/library.db`), `--embedder llama|stub` (default `llama`), `--now YYYY-MM-DD` (default: today; tests pin it).

| Command | Does | Exit codes |
|---|---|---|
| `ingest <file.jsonl> [--prune]` | Upserts every line by `id` in `id` order, in one transaction, then rebuilds `models_fts` and runs `VACUUM`. A row whose content is unchanged keeps its `indexed_at` and vectors. A changed text clears its `embed_hash` and vectors. A NULL `file_hash` in the file keeps a pinned one; a different non-NULL hash replaces it and is reported. `--prune` deletes rows of the sources present in the file that the file doesn't list, and reports them. Refuses the whole file (nothing written) on any invalid line: an unknown key, a missing field, a licence or style outside the lists, CC BY without attribution, a colour or texture word | 0 ok, 1 invalid input, 2 usage |
| `embed [--all \| --missing]` | Embeds the three texts of every row (`--all`) or of the rows whose `embed_hash` is NULL or stale (`--missing`, the default) | 0, 1, 3 GGUF mismatch |
| `search …` | §6 | 0 (even with no results), 2 usage, 3 |
| `show <id>` | The row as `key: value` lines, then the exact fetch command, e.g. `python3 -I tools/fetch_asset.py polyfork get fox-base --expect-sha256 <hash>` (with no `--expect-*` and a note to pin when the hash is NULL) | 0, 1 unknown id |
| `pin <id> <sha256>` | Records a file's hash after its first download (only where `file_hash` is NULL, or with `--force` after a reported change) | 0, 1 |
| `check` | Schema and `schema_version`; `meta` against the GGUF on disk (skipped with `--embedder stub`); every row has three vectors and a current `embed_hash`; the FTS index matches `models` (`integrity-check`); every row passes the ingest checks | 0 ok, 1 problems listed |
| `dump` | Every row of `models` sorted by `id`, one `key: value` block per row (no vectors, which are reproducible from the text and the GGUF), then `meta`. Written to `library/library.dump.txt` with each database change | 0 |
| `eval <queries.jsonl>` | Phase 4: recall@10 for vectors only, words only and RRF; the best wrong answer's `best-cos` per query; a summary table | 0 |

Every message has the form `library: …`. The program is built from `tools/library.c` on `tools/tool.h` like the other tools.

## 8. Interchange file (`.jsonl`)

There is one JSON object per line, one line per model, sorted by `id`, UTF-8, keys in the order of §3. The files are written into a scratch folder under `/tmp` (e.g. `/tmp/monolith-library/<source>.jsonl`), never into the repo.

- **Harvest** (`fetch_asset.py <source> catalog --out DIR`) writes every §3 field it knows: `id, source, name, collection, page_url, file_url, path_in_zip, file_hash, hash_kind, zip_sha256, licence, licence_url, attribution, authors, rigged, ik, tags, tris, sx, sy, sz, slots, flat_shaded, textured, factor_colours_only`, plus two fields that are not stored: `preview` (Poly Haven's thumbnail URL, Polyfork's `views.png` render URL, used only to write descriptions) and `source_tags` (raw tags kept for reading). It writes no timestamp, so a second run on an unchanged source gives a byte-identical file.
- **Describe** (phase 3) adds `category, style, style_desc, shape_desc, use_desc` to each line. The description agents work on a copy in their own scratch folder.
- **Ingest** reads the merged file. `indexed_at`, `embed_hash` and `rowid` come only from `library`.

## 9. Download on use

`fetch_asset.py <source> get` takes `--expect-md5 HEX` or `--expect-sha256 HEX` and refuses a mismatch (exit 1, nothing left in the output folder but a `REFUSED.txt` naming both hashes). `polyfork get <id>` downloads only `https://polyfork.dev/cdn/<id>.glb` (host `polyfork.dev` only, no key) and writes `provenance.json` with CC BY 4.0, the licence URL and the `credit` text. After that comes `library pin <id> <sha256>`. `kenney get <id-or-path-in-zip> --expect-zip-sha256 HEX --expect-sha256 HEX` finds the zip with that sha256 in the local cache, then extracts the entry (with the files a `.gltf` names, plus `License.txt`) into a new download folder and checks the entry hash. If no cached zip has that sha256 but one has the same pack name, the entry's sha256 decides: equal means the file is used and the newer zip reported; different means the download is refused. If the pack isn't cached at all, the command exits 1 with the pack page to download it from by hand. Then come steps 5–8 of the `assets-and-licences` skill, as before.

Request sessions (`assets-and-licences` §2–§4, `map-request` §5): `library search` → choose by description and filters → `library show <id>` → its fetch command → inspect, normalise, copy, `record_asset.py`, overrides. When no result's `best-cos` reaches the threshold, `result.md` lists the closest IDs and the developer decides what to add. There is **no web search fallback** (the developer's choice). The web `search` commands remain only for building the catalog.

## 10. Build and isolation

- Vendored: `src/vendor/sqlite/` (amalgamation, public domain), `src/vendor/sqlite-vec/` (`sqlite-vec.c`/`.h`, MIT / Apache 2.0), `src/vendor/llama.cpp/` (a pinned release: only `include/`, `src/`, `ggml/include/`, `ggml/src/` with the CPU backend, plus `LICENSE`), each with a `README.md` like xatlas's (URL, tag, commit, the files' sha256, licence, changes).
- `nob.c` builds them **only for `build/tools/library`**: objects in `build/tools-obj/library/` with a `.flags` stamp, compiled only when stale, C++ through the same path as `Xatlas`, `-w` for vendored code, CPU only (no GPU backends, no OpenMP: ggml's own thread pool). The monolith build globs `src/*.c`, which doesn't reach `src/vendor/`, so monolith, the runtime, exports and the examples are unaffected. `./build.sh` and `./build.sh --win` build the same files as before. Phase 1 proves it with `ldd` and `nm` (no `sqlite3_` or `llama_` symbols in monolith) and the build logs.
- `tools/library.c` is Linux-only, like every tool. The unit tests (`tests/unit/library_test.c`) use the stub embedder.
- `.gitattributes`: `library/library.db filter=lfs diff=lfs merge=lfs -text`. Every clone needs git-lfs (`git lfs install` once).

## 11. Open points and estimates

1. **Polyfork's "data published beside the models".** The Polyfork licence excludes it from CC BY. The catalog stores only facts (triangles, size, flags), the title and the credit line that CC BY requires, and our own descriptions. Their description, palette, style text, renders and parameters are never stored. The developer decided on 2026-11-20 that the Polyfork rows are committed with the rest.
2. **Size.** Each row holds 3 × 512 bytes of vectors, about 1 KB of text and its FTS entries: about **3–4 KB per model**, more than the brief's 2 KB. With Poly Haven (~400), every Kenney 3D pack (several thousand models; counted in phase 2) and Polyfork (3,487 free models on 2026-11-20), that is about 10 000–15 000 rows, or **30–60 MB**. That is why LFS is used, and why the dump is committed next to it.
3. **Cost of descriptions.** Writing descriptions means viewing images for every model: Kenney uses contact sheets of 9–16 models (several hundred calls), Polyfork one `views.png` per model (about 3,500 views, batched about 10 per agent call), and Poly Haven one thumbnail each. This is the most expensive part of the round, so the pilot (phase 6) measures the cost per 100 models before everything else is run.
4. Polyfork's list API has no `tags`. `tags` gets its `class`, its kit name and the words of its title.

## 12. Implementation notes (2026-11-20)

- **Vendored** (phase 1): SQLite 3.54.1, sqlite-vec v0.1.9, llama.cpp v1.0.0 (`b1b770d9`), each with its `README.md` in `src/vendor/`. They are built only into `build/tools/library` (`nob.c`, `LibraryVendor`; 239 files, about 40 s on first build). monolith's build commands are unchanged and it has no `sqlite3_` / `llama_` / `ggml_` symbols.
- **Embedding:** the GGUF's tokenizer appends `<|endoftext|>` itself; `library` checks the last token anyway. On a Ryzen 7 7435HS (CPU), embedding takes about 0.15 s per text (0.45 s per row), model load 0.2 s, one search with its query embedding 0.3 s. Two databases built from the same file are byte-identical.
- **Sources** as built (phase 2):
  - **Poly Haven:** only `/assets?t=models` and `/files/<slug>`. The list already carries every `/info` field.
  - **Polyfork:** the list pages, then `/api/assets/{id}`, paced at 1 s.
  - **Poly Pizza:** keyword sweeps (98 seed words, then the found models' tag and title words, then creators and lists), paced at 2 s. The API has no "list everything" endpoint, and many keyword searches answer HTTP 500 on its side, so some CC BY models can't be reached.
  - **Kenney:** the developer's local zip cache only (§2).
- **Pilot** (200 models, 50 per source, 52 test queries):

  | method | recall@10 | MRR |
  |---|---|---|
  | vectors | 0.977 | 0.928 |
  | words | 0.961 | 0.933 |
  | RRF | 0.992 | 0.976 |

  The best-cos of the correct answers had a median of 0.61 and a 10th percentile of 0.52. Queries with no true match in the set scored 0.39–0.49. The provisional threshold is therefore: 0.50 or more is a match, 0.45–0.50 weak, below 0.45 nothing fits. Phase 4 re-measures it on the full catalog. The developer approved the 40-model review sample.
- **Found in the pilot:**
  - Every Polyfork GLB draws nothing as downloaded: they store `COLOR_0` as VEC3, and the alpha is read as 0. `alpha_cutoff 0` on the override material works around it (a `.claude/TODOs` bug; the fix is in the importer, outside this round).
  - Some Poly Pizza files are authored in centimetres.
  - Kenney's car-kit models are smooth-shaded with a colour-map texture, so `flat_shaded` (vertices ÷ triangles ≥ 2.5) is 0 for them.
  - `fetch_asset.py` couldn't trash a refused file under `/tmp`. It now deletes it there and always writes `REFUSED.txt`.
- **`rigged` for Kenney** is 1 when `--inspect-model` reports any animation in the file. That covers the skinned characters and the per-part animated pieces (158 of the 4,753 models). The three `animated-characters-*` packs ship no glTF/GLB, so they have no rows.
- **Phase 4: search quality on the full catalog** (Poly Haven 521, Kenney 4,753, Polyfork 3,487 = 8,761 models; `tests/library/queries.jsonl`, 62 queries, each with the 1–10 models whose *name* matches, filters applied):

  | query style | weights | vectors r@10 / MRR | words r@10 / MRR | RRF r@10 / MRR |
  |---|---|---|---|---|
  | description without the object's name | 1,1,1,1 | 0.364 / 0.527 | 0.407 / 0.592 | 0.443 / 0.630 |
  | description without the object's name | 1,0.5,1,3 | 0.364 / 0.527 | 0.407 / 0.592 | 0.442 / 0.640 |
  | named (`<name>: …` in `--shape`, name in `--words`) | 1,1,1,1 | 0.628 / 0.818 | 0.909 / 0.960 | 0.864 / 0.960 |
  | named | **1,0.5,1,3** | 0.628 / 0.818 | 0.909 / 0.960 | **0.912 / 0.984** |

  - With equal weights, RRF's recall (0.864) fell below words alone (0.909). Weighting the keyword list 3 and the style list 0.5 made RRF the best in both styles, so `1,0.5,1,3` is the default (`LIB_WEIGHTS`).
  - Descriptions without the name are weak at this size. A riddle such as "two-wheeled pedal vehicle" doesn't reach the bicycle among 8,761 models. The skills therefore tell request sessions to name the object.
  - Absolute numbers understate the search: an expected set holds every model whose name matches, so near-duplicates and variants compete for the ten places.
- **"Nothing fits":**

  | | best-cos |
  |---|---|
  | correct answers to named queries: minimum | 0.46 |
  | 10th percentile | 0.58 |
  | 25th percentile | 0.61 |
  | median | 0.65 |
  | 11 objects not in the catalog (drum kit, ferris wheel, igloo, telescope, zeppelin, sewing machine …), top result | 0.48–0.59 |

  The ranges overlap, so `best-cos` is only a guide: 0.60 or more is usually the object, 0.50–0.60 is decided by reading the result's category, below 0.50 nothing fits. What tells them apart is the category: for a missing object the top results are look-alikes of another kind (a car wheel for "ferris wheel", a thimble for "sewing machine"). The skills say so.
- **Size:** 28 MB for 8,761 rows, about 3.2 KB per row.
- **Poly Pizza's request quota.** The developer's key is on the free plan: **3,000 API requests a month**, failed ones included. A used-up quota answers HTTP 401, the same as a wrong key. The first full sweep spent a whole key's month without saving anything, so two things changed: `polypizza catalog --max-requests` (default 2,000) ends the sweep normally at its budget, and a sweep that stops early writes what it found to `polypizza.partial.jsonl`. The sweep on 2026-11-20 used 2,000 requests: 98 seed keywords found 5,899 models, and tag words brought the total to **8,081** (2,854 CC0, 5,227 CC BY 3.0). That leaves about 900 requests for `polypizza get` until the quota resets (Dec 20). Next month's quota can extend the catalog by running the sweep again and ingesting the new rows.
- **Licence spelling.** `polyfork get` writes `CC-BY 4.0` into `provenance.json`, the spelling `record_asset.py` accepts; `CC BY 4.0` was refused. The catalog keeps the `CC BY 4.0` spelling of §3.
- **End-to-end run (phase 5)** on a scratch copy of `maps/pine_forest`:
  1. `library search --shape "bench: …" --use "campsite, forest clearing" --words bench --style-tag lowpoly` returned `polyfork-split-log-bench-289780` first, in 0.36 s.
  2. `show` printed the fetch command without a hash. After the first download, `pin` recorded the hash, and `show` then printed `--expect-sha256`.
  3. Fetching again with that hash exited 0. A wrong hash exited 1 and left only `REFUSED.txt`.
  4. `--inspect-model` measured 686 triangles and 2 × 0.45 × 0.52 m. `record_asset.py` wrote the sidecar and the `ASSETS.md` row with the CC-BY 4.0 credit.
  5. The model was placed with an `alpha_cutoff 0` override. In a `--shot` from the "Campfire clearing" viewpoint, the bench is the only change: 11,770 pixels, inside its own box and shadow.
- **Final catalog** (2026-11-20): **16,842 models**: Poly Pizza 8,081, Kenney 4,753 (47 packs), Polyfork 3,487, Poly Haven 521. `library check` passes. `library.db` is 51 MB in LFS, about 3.0 KB per row; `library.dump.txt` is 20 MB of text.
- **Final measurement** on all four sources. The queries were generated once (a scratch script, not kept): each is phrased by hand without reading the catalog's descriptions, its expected set is every model whose name matches a pattern (some patterns with exclusions, e.g. bike racks for "bicycle"), and a query matching more than 10 models is split into one query per source. That gives **139 queries**: `tests/library/queries.jsonl` (named) and `tests/library/queries-unnamed.jsonl` (descriptions without the name).

  | queries | vectors r@10 / MRR | words r@10 / MRR | RRF r@10 / MRR |
  |---|---|---|---|
  | named, weights 1,0.5,1,3 | 0.685 / 0.779 | 0.884 / 0.929 | **0.921 / 0.932** |
  | without the name, weights 1,0.5,1,3 | 0.461 / 0.571 | 0.486 / 0.592 | **0.524 / 0.662** |

  RRF beats both single methods in both query styles. One search, query embedding included, takes about 0.35 s on CPU.
- **Models to avoid:** Poly Pizza holds fan copies of brands, franchise characters and real people (Mario, LEGO, Coca-Cola, a Death Star, a Ferrari F40, a politician's caricature …), uploaded as CC0 or CC BY. The licence doesn't cover the trademark or the person. `assets-and-licences` §3 tells request sessions never to choose them; they stay in the catalog because a keyword filter would remove apples and androids too.
