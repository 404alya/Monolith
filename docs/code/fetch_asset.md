# `tools/fetch_asset.py`: licence-safe asset downloads

`tools/fetch_asset.py` downloads models and textures for monolith map packages, licence-safe. It uses the Python standard library only. The workflow around it (choosing, checking and recording assets) is the `assets-and-licences` skill (`.claude/skills/assets-and-licences/SKILL.md`).

## Running it

```
python3 -I tools/fetch_asset.py <source> <command> ...
```

`-I` keeps the current directory and the user site-packages off `sys.path`.

## Sources and commands

| Command | What it does |
|---|---|
| `polyhaven search [text] [--category c] [--max-tris n] [--limit n]` | Searches CC0 models (API). |
| `polyhaven get <slug> [--res 1k\|2k\|4k] [--out DIR]` | glTF + `.bin` + textures, md5-verified. |
| `ambientcg search <text> [--limit n]` | Searches CC0 materials (API). |
| `ambientcg get <id> [--res 1K\|2K] [--out DIR]` | The texture zip; extracts image files only. |
| `kenney list <zip url> [--out DIR]` | Downloads a pack zip, audits it, lists its entries. |
| `kenney get <zip url> --pick GLOB [--pick ...] [--out DIR]` | Extracts the picked files + `License.txt`. |
| `polypizza search <keyword> [--license cc0\|ccby] [--category n] [--max-tris n] [--limit n]` | Searches Poly Pizza. |
| `polypizza info <id>` | Shows one model's details. |
| `polypizza get <id> [--out DIR]` | The GLB; only CC0 / CC-BY, recorded at download. |

## Safety rules built in

These are the project's hard rules for downloads:

- **A new, empty directory.** Every download goes into a new, empty directory outside the repo: by default under `$MONOLITH_DOWNLOADS` or `~/.cache/monolith/downloads`, then `<source>-<id>-<stamp>/`.
- **Network.** https only, redirects only to the source's own hosts, and a size cap per file.
- **Archives.** Archive entries are audited (absolute paths, `..`, backslashes, symlinks, sizes), and only model, texture and licence files are written. Nothing from a download is ever executed.
- **Provenance.** Every download writes `provenance.json` next to the files: source, URLs, author(s), licence + URL, date, md5 (when the API gives one) and sha256 of every file.
- **Poly Pizza** only through its official API v1.1 (`https://api.poly.pizza/v1.1`, header `x-auth-token`), with the developer's key from `$POLY_PIZZA_API_KEY` or `~/.config/monolith/poly-pizza.key`. The key is never printed or written anywhere.

## Environment

| Variable | Use |
|---|---|
| `MONOLITH_DOWNLOADS` | The downloads folder (default `~/.cache/monolith/downloads`). |
| `POLY_PIZZA_API_KEY` | The Poly Pizza API key (else `~/.config/monolith/poly-pizza.key`). |
