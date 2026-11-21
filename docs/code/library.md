# `tools/library.c`: the model catalog

`build/tools/library` builds, checks and searches the model catalog `library/library.db`: one row per model from Poly Haven, Kenney and Polyfork with its links, licence, file hash, measured facts and our descriptions, three embedding vectors per row and a keyword index. The design (sources, licences, schema, descriptions, embeddings, search, the interchange file) is [model-library.md](../model-library.md); this page is the reference of the program. For people: [library/README.md](../../library/README.md). Overview of all the tools: [architecture.md §8](../architecture.md#8-tools).

It has no network access: the harvester and the downloader are `tools/fetch_asset.py`. It is the only program that links SQLite, sqlite-vec and llama.cpp (vendored in `src/vendor/`, built by `nob.c`'s `LibraryVendor`; [nob.md](nob.md)). Linux only, like every tool.

## Building it

```
./build.sh --tools library
```

The first build compiles the vendored code (239 files, in parallel; about 40 s on 16 threads) into `build/tools-obj/library/` and packs it into `libvendor.a`; later builds compile only what is stale. After that the program keeps itself current: when `tools/library.c`, `tools/tool.h` or nob.h is newer than the binary it runs `./build.sh --tools library` (output on stderr) and then itself again (`ToolInitBuilt` in `tools/tool.h`).

## Command line

```
build/tools/library [--db PATH] [--embedder llama|stub] [--now YYYY-MM-DD] [--timing] <command> [arguments]
```

The global flags may stand anywhere on the line.

| Flag | Meaning |
|---|---|
| `--db PATH` | The database (default `<repo>/library/library.db`). Tests and scratch work use databases under `/tmp`. |
| `--embedder llama\|stub` | `llama` (default): Qwen3-Embedding-0.6B through llama.cpp, the GGUF checked first. `stub`: a vector from a SHA-256 stream of the text, no model needed (tests). A database remembers which one made its vectors (`meta.embedder`); `search`, `eval` and `embed --missing` refuse vectors of the other one. |
| `--now YYYY-MM-DD` | The date written into `indexed_at` (default: today, local time). Tests pin it. |
| `--timing` | On stderr: the model load, the query embedding and the whole search (`search`), the model load (`embed`). |

| Command | Does | Exit codes |
|---|---|---|
| `ingest <file.jsonl> [--prune]` | Reads every line (format: [model-library.md §8](../model-library.md#8-interchange-file-jsonl)) and checks it; then upserts the rows by `id`, in `id` order, in one transaction, rebuilds the keyword index and runs `VACUUM`. A row whose content is unchanged keeps its `indexed_at` and vectors; a changed row gets `--now` as `indexed_at`; a change to an embedded text (category, name, style or a description) also deletes its vectors and clears `embed_hash`. A `file_hash` of `null` in the file keeps a hash pinned in the database; a different non-null hash replaces it and is reported. `--prune` deletes, and reports, the rows of the sources present in the file that the file doesn't list. Any invalid line refuses the whole file and nothing is written; when nothing changed nothing is written either (the database keeps its bytes). | 0, 1 invalid input or database error, 2 usage |
| `embed [--all \| --missing]` | Embeds the three texts of every row (`--all`) or of the rows whose `embed_hash` is NULL or no longer matches their texts (`--missing`, the default); commits every 64 rows (progress on stderr), then sets `meta.embedder` and runs `VACUUM`. With nothing to do it writes nothing. | 0, 1, 2, 3 GGUF missing or different |
| `search …` | Below. | 0 (also with no results), 1 no database or vectors of the other embedder, 2 usage, 3 GGUF |
| `show <id>` | The row as `key: value` lines (schema order; `NULL` for NULL), then `fetch: <command>`, and a `note:` to pin when the row has no hash. | 0, 1 unknown id |
| `pin <id> <sha256> [--force]` | Records a file's sha256 (`hash_kind` `sha256`) after its first download, where the row has no hash; `--force` replaces a different one (reported). `indexed_at` becomes `--now`. The same hash again is a no-op. | 0, 1 (unknown id, not 64 lower-case hex digits, a different hash without `--force`), 2 |
| `check` | Lists every problem: the schema objects against schema version 1 (their SQL text), `meta` (every key, `schema_version` 1); the GGUF on disk against `meta.embed_sha256` and `meta.embedder` = `llama` (skipped with `--embedder stub`); per row the ingest checks, three vectors and a current `embed_hash`; vectors of rows that are gone; the keyword index against `models` (FTS5 `integrity-check`); `PRAGMA integrity_check`. | 0 none, 1 problems |
| `dump` | Every row by `id` as a block of `key: value` lines (no vectors: they follow from the texts and the GGUF) with a blank line after each, then `meta.<key>: value` lines by key. This is `library/library.dump.txt`. | 0, 1 |
| `eval <queries.jsonl>` | Below. | 0, 1 invalid file or database, 2, 3 |

Every message is `library: …` on stderr; data (search lines, `show`, `dump`, `eval`'s tables) goes to stdout.

### search

```
library search [--shape TEXT] [--style TEXT] [--use TEXT] [--words TEXT] [--style-tag T]... [--source S]...
               [--max-tris N] [--max-size M] [--cc0-only] [--limit N] [--weights s,t,u,w]
```

At least one of `--shape`, `--style`, `--use`, `--words` (else exit 2).

1. **Filters**, in SQL on `models`: `--style-tag` (repeatable; one of `lowpoly stylised realistic photoscan voxel toon`), `--source` (repeatable; `polyhaven kenney polyfork polypizza`), `--max-tris N` (`tris <= N`), `--max-size M` (the largest of `sx sy sz` ≤ M, in file units), `--cc0-only` (`licence = 'CC0 1.0'`). A row with NULL `tris`, or any NULL size, passes those two filters; its output shows `?`.
2. **Vector lists**, one per given text (`--shape`, `--style`, `--use`): the query is embedded with its instruction (model card format `Instruct: <task>\nQuery:<text>`, the tasks in `meta.instruct_*`), then the exact cosine distance to every filtered row with vectors (`vec_distance_cosine` in a full scan); the top 50 by distance, then id.
3. **Keyword list**: FTS5 `bm25` over `models_fts` (category, tags, the descriptions, name); the query is the words of `--words`, or without it of `--shape` and `--use`: runs of letters and digits (and multi-byte characters), lower case, each once, sorted, each quoted as an FTS5 string, joined by `OR` (no FTS5 syntax from the text gets through). Top 50 by bm25, then id.
4. **RRF**: `score = Σ weight / (60 + rank)` over the lists that ran (rank from 1), weights `--weights shape,style,use,words` (default `1,0.5,1,3`, measured in phase 4: [model-library.md](../model-library.md) §12). `eval --weights` replaces every query's weights.
5. Sorted by score descending, then id; the first `--limit` (default 10) printed as tab-separated lines:

```
id  score(6 decimals)  best-cos(4 decimals)  tris|?  sx×sy×sz|?  category  shape_desc
```

`best-cos` is the highest cosine similarity of that row to the query vectors (computed for the row itself, also when it is outside a list's top 50), `-` when no vector list ran. Rows without vectors (not embedded since they changed) appear only through the keyword list, with a warning on stderr. When the database has no vectors at all, the vector texts are skipped with a warning and the model isn't loaded.

### eval

`eval <queries.jsonl>` measures search quality on a set of queries with known answers (phase 4 of the round sets the "nothing fits" threshold from it). One JSON object per line; blank lines are skipped:

```json
{"q": "pine-forest-edge", "shape": "tall narrow conifer", "style": "faceted low poly", "use": "forest edge",
 "words": "pine tree", "filters": {"style_tag": ["lowpoly"], "source": "kenney", "max_tris": 2000,
 "max_size": 10, "cc0_only": true}, "expect": ["kenney-nature-kit/tree_pineTallA"]}
```

| Key | Value |
|---|---|
| `q` | the query's name in the output (required) |
| `shape`, `style`, `use`, `words` | the texts of `search`'s flags of the same name; string or `null`; at least one |
| `filters` | optional object: `style_tag`, `source` (a string or a list of strings), `max_tris` (integer), `max_size` (number), `cc0_only` (boolean). Any other key is an error |
| `expect` | the ids that are right answers (a non-empty list of strings, or one string) |

Any unknown key or wrong type refuses the file (exit 1). Every query runs three rankings from the same lists: **vectors** (RRF of the vector lists only), **words** (the keyword list only) and **rrf** (all of them, as `search`). The output is tab-separated: a header, one line per query, a blank line, then the summary:

```
q  vec_r@10  words_r@10  rrf_r@10  vec_rank  words_rank  rrf_rank  expected_cos  wrong_cos
...
method   queries  recall@10  mrr
vectors  …
words    …
rrf      …
lowest expected best-cos   0.6211
highest wrong best-cos     0.5402
```

- `*_r@10`: the share of the expected ids in that ranking's first 10; `-` when the ranking had no list (no vector text, or no words).
- `*_rank`: the position of the first expected id in that ranking (up to the union of the top-50 lists), `-` when none is in it.
- `expected_cos`: `best-cos` of the best-ranked expected id in the rrf ranking; `wrong_cos`: the highest `best-cos` among the ids in the rrf top 10 that are not expected (`-` without vector lists).
- Summary: per ranking the queries it ran on, mean recall@10 and mean reciprocal rank (0 for a miss); the lowest `expected_cos` and the highest `wrong_cos` over all queries, the two numbers a threshold must separate.

## Embeddings

Qwen3-Embedding-0.6B, GGUF Q8_0 ([library/EMBEDDING.md](../../library/EMBEDDING.md)), on the vendored llama.cpp, CPU only, with fixed settings so a text gives the same bytes on every run: 4 threads, context, batch and micro-batch 2048 tokens, one sequence, embeddings on with last-token pooling (`LLAMA_POOLING_TYPE_LAST`), flash attention off, no GPU layers. Each text is tokenised with the model's special tokens added but without parsing special tokens inside the text (a description can't inject one), and must end in `<|endoftext|>`: the GGUF's tokenizer appends it itself (`embed` says so on stderr; `library` would append it otherwise). Each text is decoded alone with the memory cleared first. The pooled vector's first 512 of 1024 dimensions are L2-normalised and quantised per vector: `q = round(127 · x / max|x|)` (`meta.quantisation` `maxabs-127`).

Texts per row: shape `"<category>: <name>. <shape_desc>"`, style `"<style>. <style_desc>"`, use `"<category>. <use_desc>"`; documents get no instruction. `embed_hash` is the sha256 of the three joined by `\n\0`.

**The GGUF check.** The first time a run needs the model, the file `meta.embed_file` in `$MONOLITH_EMBED_DIR` (default `~/.cache/monolith/embed/`) is hashed and compared with `meta.embed_sha256` (a run without a database uses the built-in values). The hash is cached in `<file>.sha256` next to it, keyed by size and modification time, so only the first run after a change reads the 600 MB. Missing or different: exit 3 with the fetch command.

**The stub** (`--embedder stub`): 512 values from SHA-256(text ‖ block number), 32 per block, `(byte − 127.5) / 127.5`, then the same normalisation and quantisation. It ignores the query instruction, so a query equal to a row's embedded text finds that row with best-cos 1. The same text always gives the same vector; different texts give unrelated ones.

## Ingest checks

A line is refused for: a key that isn't a column of the schema (except `preview` and `source_tags`, which are read and ignored; `indexed_at`, `embed_hash` and `rowid` come only from `library`); a key given twice; a value of the wrong type (text, integer, a 0/1 flag (JSON `true` / `false` too), a number); a missing or empty required field (`id source name page_url file_url licence licence_url category style style_desc shape_desc use_desc tags`); a control character in a text; a source other than `polyhaven kenney polyfork polypizza`, or an id that doesn't start with `<source>-`, or holds a blank; a licence other than `CC0 1.0` / `CC BY 4.0` / `CC BY 3.0` (Poly Pizza), or CC BY without `attribution`; a style outside the list; a link that isn't `https://`; `file_hash` without `hash_kind` or the other way round, a hash that isn't lower-case hex of its kind's length (md5 32, sha256 64), a bad `zip_sha256`; a Kenney row without `path_in_zip`; upper case in `category` or `tags`; a negative count or size, a flag other than 0 / 1; a description (`style_desc`, `shape_desc`, `use_desc`) of fewer than two words (a code, not a description); a colour, material or texture word in `category`, `shape_desc`, `style_desc` or `use_desc` (`BANNED_WORDS` in `tools/library.c`, whole words, any case: the one list, used by `ingest`, `check` and the unit test); and an id on two lines. Every problem of the file is listed (`library: ingest: <file>: line <n>: <id>: <problem>`).

## show's fetch command

| Source | Command |
|---|---|
| Poly Haven | `python3 -I tools/fetch_asset.py polyhaven get <slug> --expect-md5 <file_hash>` |
| Kenney | `python3 -I tools/fetch_asset.py kenney get <id> --expect-sha256 <file_hash> --expect-zip-sha256 <zip_sha256>` (from the local zip cache, `~/.cache/monolith/kenney/`) |
| Polyfork | `python3 -I tools/fetch_asset.py polyfork get <asset id> --expect-sha256 <file_hash>` |
| Poly Pizza | `python3 -I tools/fetch_asset.py polypizza get <model id> --expect-sha256 <file_hash>` |

The hash flag follows `hash_kind` and is left out when the row has no hash (then a `note:` line says to `pin` the hash from `provenance.json` after the download). Words a shell would split or expand are single-quoted.

## Environment

| Variable | Use |
|---|---|
| `MONOLITH_EMBED_DIR` | The folder of the GGUF and its hash cache (default `~/.cache/monolith/embed`). |
| `HOME` | The default folder above. |
| `CC`, `CXX`, `AR` | The compilers and archiver of the build (`./build.sh --tools library`; defaults `clang`, `clang++`, `ar`). |

`library` itself sets `GGML_NO_BACKTRACE=1` before loading the model, so an abort inside ggml doesn't start a debugger.

## Files

| File | |
|---|---|
| `tools/library.c` | the program (`LIBRARY_NO_MAIN` leaves out `main` for the unit test) |
| `library/library.db` | the catalog (Git LFS, `.gitattributes`); `library/library.dump.txt` its `dump`, committed with each change |
| `library/EMBEDDING.md`, `library/README.md` | the embedding model's record; the guide for people |
| `src/vendor/sqlite/`, `src/vendor/sqlite-vec/`, `src/vendor/llama.cpp/` | the vendored libraries, each with its `README.md` |
| `build/tools-obj/library/` | their objects, `gen/` (llama.cpp's version headers), `libvendor.a` |
| `tests/unit/library_test.c`, `tests/library/catalog.jsonl` | the unit test and its fixture |

## Callers

- Request sessions choosing a model: `search`, then `show <id>` and its fetch command, then `pin` for a Polyfork model ([model-library.md §9](../model-library.md#9-download-on-use); the `assets-and-licences` and `map-request` skills).
- Building the catalog: `fetch_asset.py <source> catalog` writes the `.jsonl`, the description agents add the texts, then `ingest`, `embed`, `check`, `dump > library/library.dump.txt`; `eval` on the query set.
- `./build.sh test library` (`tests/unit/library_test.c`, ASan + UBSan, stub embedder): the schema, byte-identical ingests, changed texts, pinned hashes, refused files, RRF on a hand-made example, the filters, int8 quantisation, stable output and tie order, `show`'s fetch commands, the word list, SHA-256, `eval`, the exit codes.
- `build/tools/leakcheck` ([leakcheck.md](leakcheck.md)): every command under LSan with the stub embedder, and the real model when its GGUF is on disk.
