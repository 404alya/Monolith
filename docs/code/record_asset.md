# `tools/record_asset.py`: record an asset's provenance

`tools/record_asset.py` writes an asset's provenance into a map package: the `<file>.asset.md` sidecar and its row in `ASSETS.md` (created or replaced). It uses the Python standard library only; run it with `python3 -I`. The workflow around it is the `assets-and-licences` skill.

## With a provenance.json

For a file downloaded with `tools/fetch_asset.py` ([fetch_asset.md](fetch_asset.md)):

```
python3 -I tools/record_asset.py --package maps/<name> --file models/x.glb \
    --provenance <download dir>/provenance.json [--original <path in provenance>]... \
    [--inspect <file with the output of monolith --inspect-model>] \
    [--normalize-report <json from tools/normalize_model.py --report>] \
    [--change "text"]... [--ai-generated yes|no|unknown] [--note "text"]
```

## Without a provenance.json

For a file copied from another project, or a local file the developer supplied, describe the source by hand instead of `--provenance`:

```
python3 -I tools/record_asset.py --package maps/<name> --file models/x.glb \
    --source-note "copied from <project>/assets/x.glb (another project)" \
    --licence "CC0 1.0" [--licence-url URL] [--author NAME]... [--attribution TEXT] \
    [--source-url URL] [--original-file <path of the original, hashed>]... [other options]
```

- `--licence` takes CC0 / CC-BY / public domain, as with a provenance, or `unknown`: recorded as unknown, with a warning; the developer decides.
- The project's licence evidence (a `License.txt`, its `ASSETS.md` / README) goes into `--note`.

## What it writes

- `--file` is package-relative and must already exist (copy the checked file in first).
- The sidecar is `<package>/<file>.asset.md`.
- `ASSETS.md` gets one row per file: the row whose first cell is `` `<file>` `` is replaced, else a row is appended to the table.
- The changes listed are the changes made to the **file** (normalisation, conversion). Restyling through material overrides in the map is not a change of the file; it is described in the map and `MAP.md` instead.
