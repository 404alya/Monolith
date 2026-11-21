# `tools/fetch_asset.py`: licence-safe asset downloads

`tools/fetch_asset.py` downloads models and textures for monolith map packages, licence-safe, and harvests the model library's catalog files (`docs/model-library.md`). It uses the Python standard library only. The workflow around it (choosing, checking and recording assets) is the `assets-and-licences` skill (`.claude/skills/assets-and-licences/SKILL.md`).

## Running it

```
python3 -I tools/fetch_asset.py <source> <command> ...
```

`-I` keeps the current directory and the user site-packages off `sys.path`.

## Sources and commands

| Command | What it does |
|---|---|
| `polyhaven search [text] [--category c] [--max-tris n] [--limit n]` | Searches CC0 models (API). |
| `polyhaven get <slug> [--res 1k\|2k\|4k] [--expect-md5 HEX \| --expect-sha256 HEX] [--out DIR]` | glTF + `.bin` + textures, md5-verified against the API. `--expect-*` checks the `.gltf` before anything else is downloaded. |
| `polyhaven catalog --out DIR [--limit N]` | Writes `DIR/polyhaven.jsonl`: every model of `/assets?t=models` (which carries every `/info` field) with `/files/<slug>` for the 1k glTF URL and its md5. Models without a 1k glTF or an md5 are skipped and listed. |
| `ambientcg search <text> [--limit n]` | Searches CC0 materials (API). |
| `ambientcg get <id> [--res 1K\|2K] [--out DIR]` | The texture zip; extracts image files only. |
| `kenney list [PACK]` | The packs in the local zip cache (slug, zip sha256, zip), or one pack's models with their paths in the zip. |
| `kenney catalog [ZIP...] --out DIR [--monolith PATH]` | With no zips, every `*.zip` in the cache. Per pack: audits it, keeps one entry per model (the `GLTF format` folder, then `GLB format`, then any other), records its path in the zip and sha256 and the cached zip's sha256, and measures it with `monolith --inspect-model` (triangles, size, slots, textured, colour factors only, `flat_shaded` = vertices ÷ triangles ≥ 2.5) in a scratch download folder that is moved to the trash afterwards. The cached zips are only read. `page_url` and `file_url` are `https://kenney.nl/assets/<pack>` (never fetched). Rows merge into `DIR/kenney.jsonl`, replacing the rows of the same packs. `--monolith` defaults to this checkout's `build/linux/monolith`. |
| `kenney get <path-in-zip \| kenney-<pack>/<name>> [--pack PACK] --expect-zip-sha256 HEX --expect-sha256 HEX [--out DIR]` | Finds the cached zip with that sha256 and extracts the entry, every file it names (a `.gltf`'s or `.glb`'s buffers and images) and the pack's `License.txt` into a new download folder with `provenance.json`, then checks the entry's sha256. If no cached zip has that sha256 but one holds the same pack (pass `--pack` with a path), the entry's hash decides: equal means it is used and the newer zip reported; different means it is refused. A pack that isn't cached fails (exit 1) naming the page to download it from by hand. |
| `polypizza search <keyword> [--license cc0\|ccby] [--category n] [--max-tris n] [--limit n]` | Searches Poly Pizza. |
| `polypizza info <id>` | Shows one model's details. |
| `polypizza get <id> [--expect-sha256 HEX] [--out DIR]` | The GLB; only CC0 / CC-BY, recorded at download. `--expect-sha256` (or `--expect-md5`) refuses a different file. |
| `polypizza catalog --out DIR [--limit N] [--max-words N] [--estimate]` | Writes `DIR/polypizza.jsonl` through the official API v1.1, at least 2 s between requests. `/search/<keyword>` needs a keyword, pages with `Page` (from 0) and `Limit` (at most 32) and reports `total`; many keywords answer HTTP 500 for the whole query, so each keyword is searched per licence (`License` 1 = CC0, 0 = CC-BY) and, where that fails, per licence and `Category`. The sweep runs fixed seed keywords, then the models' own tag and title words (most frequent first, up to `--max-words`), then every creator found (`/user/<name>`: their 32 newest models and their lists) and every list (`/list/<id>`), deduplicated by id. It prints how many models each phase found and how many queries failed. Rows: CC0 1.0, or CC BY 3.0/4.0 with the API's attribution text; other licences and CC BY without attribution are skipped and counted; no hash (the API publishes none), no size. `--limit N` stops the seed sweep at N models and keeps the first N by id; `--estimate` reads only the first page of each seed keyword and prints the pages a full sweep of them needs. `--max-requests N` (default 2000) ends the sweep normally after N API requests: the free plan allows **3000 requests a month per key** (failed ones count), shared with `polypizza get`, and a 401 is what a used-up quota looks like. A 401 or 403 (or giving up after repeated 429s) stops it with a message; the models found until then are written to `DIR/polypizza.partial.jsonl` first, and it exits 1. |
| `polyfork catalog --out DIR [--limit N]` | Writes `DIR/polyfork.jsonl`: every free model of `/api/assets?free=1` (pages of 100), then `/api/assets/<id>` for the material count, the `credit` line and the `views.png` render URL (`preview`). Rows are CC BY 4.0 with the credit as attribution and no hash (the API publishes none). |
| `polyfork get <id> [--expect-sha256 HEX] [--out DIR]` | Only the free GLB, `https://polyfork.dev/cdn/<id>.glb` (no key), after checking that `/api/assets/<id>` still offers it under CC BY 4.0; `provenance.json` records the licence and the credit. Without `--expect-sha256` it prints the `library pin` command with the file's hash. |
| `huggingface get <repo> <file> --expect-sha256 HEX [--revision REV] [--out DIR]` | Only for the model library's embedding model. Downloads one file into `$MONOLITH_EMBED_DIR` or `~/.cache/monolith/embed/` (that folder keeps other files: it writes `<file>` and `<file>.provenance.json`, and refuses to replace either). The size cap is 1 GiB for this source. |

`--limit N` on a `catalog` takes the first N models by ID (for pilot runs). Catalog files go into a scratch folder outside the repo (e.g. `/tmp/monolith-library/`): one JSON object per line, sorted by `id`, keys in the order of `docs/model-library.md` §8, no timestamps, so a second run on an unchanged source writes the same bytes. Each catalog command ends with its request count per host and its time.

**Kenney: the local zip cache.** No command fetches from kenney.nl (on 2026-11-20 its `robots.txt` disallowed Claude's agents site-wide, and the pack pages gave only browser-signed zip links). The developer downloads pack zips, or the All-in-1 bundle, in a browser into `~/.cache/monolith/kenney/` (`MONOLITH_KENNEY_ZIPS` overrides it). A pack zip has `License.txt` at its top and its pack comes from its name (`kenney_<pack>.zip`, a version suffix is ignored). In a bundle, each nested `kenney_<pack>.zip` is a pack, and so is each folder with a `License.txt` and `.glb`/`.gltf` models below it (pack from the folder's name, e.g. `Car Kit` → `car-kit`); a nested pack's `path_in_zip` is `<inner zip>!/<path in it>`. A zip whose pack can't be told is skipped and named. When a pack is in two cached zips, the pack zip wins over a bundle. The zips' hashes are kept in `<cache>/.sha256-index` (keyed by name, size and mtime), so an unchanged zip is hashed once.

## Safety rules built in

These are the project's hard rules for downloads:

- **Expected hashes.** Every `get` takes `--expect-md5 HEX` or `--expect-sha256 HEX` (`huggingface get` requires `--expect-sha256`, `kenney get` both `--expect-sha256` and `--expect-zip-sha256`). A mismatch is refused: exit 1, the downloaded files moved to the trash with `gio trash` (deleted instead when the folder is under `/tmp`, where gio can't trash), and a `REFUSED.txt` (written even if moving them fails) (for `huggingface`, `<file>.REFUSED.txt`) left in the folder naming the expected and the actual hash.
- **Pacing.** Requests to one host are at least 1 s apart, with the user agent that names monolith. On HTTP 429 the tool waits as `Retry-After` says (60 s when it is missing), doubles the wait on each further 429, and gives up after four tries with a message.
- **A new, empty directory.** Every download goes into a new, empty directory outside the repo: by default under `$MONOLITH_DOWNLOADS` or `~/.cache/monolith/downloads`, then `<source>-<id>-<stamp>/`.
- **Network.** https only, redirects only to the source's own hosts, and a size cap per file (200 MB; 1 GiB for `huggingface`). The hosts per source are listed in `HOSTS` in the script: `polyfork` is `polyfork.dev` only; `huggingface` is `huggingface.co` and the CDN its `resolve/` links redirect to (`us.aws.cdn.hf.co`, checked by hand).
- **Archives.** Archive entries are audited (absolute paths, `..`, backslashes, symlinks, sizes), and only model, texture and licence files are written. Nothing from a download is ever executed.
- **Provenance.** Every download writes `provenance.json` next to the files: source, URLs, author(s), licence + URL, date, md5 (when the API gives one) and sha256 of every file.
- **Poly Pizza** (search, get and catalog) only through its official API v1.1 (`https://api.poly.pizza/v1.1`, header `x-auth-token`), with the developer's key from `$POLY_PIZZA_API_KEY` or `~/.config/monolith/poly-pizza.key`. The key is never printed or written anywhere.

## Environment

| Variable | Use |
|---|---|
| `MONOLITH_DOWNLOADS` | The downloads folder (default `~/.cache/monolith/downloads`). |
| `MONOLITH_KENNEY_ZIPS` | The Kenney zip cache (default `~/.cache/monolith/kenney`). |
| `MONOLITH_EMBED_DIR` | The embedding model's folder for `huggingface get` (default `~/.cache/monolith/embed`). |
| `POLY_PIZZA_API_KEY` | The Poly Pizza API key (else `~/.config/monolith/poly-pizza.key`). |
