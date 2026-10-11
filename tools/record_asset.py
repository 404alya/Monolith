#!/usr/bin/env python3
# record_asset.py -- write an asset's provenance into a map package: the <file>.asset.md sidecar
# and its row in ASSETS.md (created or replaced). Stdlib only; run with `python3 -I`.
#
#   python3 -I tools/record_asset.py --package maps/<name> --file models/x.glb \
#       --provenance <download dir>/provenance.json [--original <path in provenance>]... \
#       [--inspect <file with the output of monolith --inspect-model>] \
#       [--normalize-report <json from tools/normalize_model.py --report>] \
#       [--change "text"]... [--ai-generated yes|no|unknown] [--note "text"]
#
# --file is package-relative and must already exist (copy the checked file in first). The
# sidecar is <package>/<file>.asset.md; ASSETS.md gets one row per file (the row whose first
# cell is `<file>` is replaced, else a row is appended to the table). Changes listed are the
# changes made to the FILE (normalisation, conversion); restyling through material overrides
# in the map is not a change of the file and is described in the map and MAP.md instead.

import argparse
import datetime
import json
import pathlib
import re
import sys

HEADER = "| File | Source URL | Author | Licence (+ URL) | Downloaded | sha256 of the original | Changes |"
RULE = "|---|---|---|---|---|---|---|"
MODEL_EXT = {".glb", ".gltf", ".obj", ".fbx", ".dae"}


def Fail(msg):
    print("record_asset: " + msg, file=sys.stderr)
    sys.exit(1)


def Cell(s):
    return str(s).replace("|", "\\|").replace("\n", " ")


def ParseInspect(text):
    """Pick the useful lines out of `monolith --inspect-model` output."""
    out = {}
    for key in ("format", "generator", "extensionsUsed", "extensionsRequired", "triangles",
                "size", "up axis", "units", "animations"):
        m = re.search(r"^%s: (.*)$" % re.escape(key), text, re.M)
        if m:
            out[key] = m.group(1).strip()
    out["materials"] = re.findall(r"^  \[(\d+)\] (.*)$", text, re.M)
    out["maps"] = re.findall(r"^      ((?:albedo|normal|roughness|metalness|occlusion|emission|orm)[a-z ]* map: .*)$",
                             text, re.M)
    w = text.split("\nwarnings:\n", 1)
    out["warnings"] = [l[4:] for l in w[1].split("\n") if l.startswith("  - ")] if len(w) == 2 else []
    return out


def main():
    p = argparse.ArgumentParser(description="Record an asset's provenance in a map package")
    p.add_argument("--package", required=True)
    p.add_argument("--file", required=True, help="package-relative path, e.g. models/pine.glb")
    p.add_argument("--provenance", required=True)
    p.add_argument("--original", action="append", default=[],
                   help="path (as in provenance.json) of the original file(s) this one came from; "
                        "default: every model or texture file in the provenance")
    p.add_argument("--inspect")
    p.add_argument("--normalize-report")
    p.add_argument("--change", action="append", default=[])
    p.add_argument("--ai-generated", choices=["yes", "no", "unknown"], default="unknown")
    p.add_argument("--note", action="append", default=[])
    a = p.parse_args()

    pkg = pathlib.Path(a.package)
    rel = pathlib.PurePosixPath(a.file)
    if rel.is_absolute() or ".." in rel.parts:
        Fail("--file must be package-relative")
    target = pkg / rel
    if not target.is_file():
        Fail("%s does not exist: copy the checked file into the package first" % target)
    assets = pkg / "ASSETS.md"
    if not assets.is_file():
        Fail("%s missing (every package has one; --new-map creates it)" % assets)
    prov = json.loads(pathlib.Path(a.provenance).read_text())
    for k in ("source", "licence", "licenceUrl", "files", "downloadedAt"):
        if k not in prov:
            Fail("provenance has no %r" % k)
    lic = prov["licence"]
    if not re.match(r"(?i)^(cc0|cc-?by [0-9.]+$|cc-?by$|public domain)", lic):
        Fail("licence %r is not CC0 / CC-BY / public domain: such an asset must not be used" % lic)
    ccby = lic.lower().replace(" ", "").startswith(("cc-by", "ccby"))
    if ccby and not prov.get("attribution"):
        Fail("CC-BY asset without an attribution text in the provenance")

    files = prov["files"]
    if a.original:
        orig = [f for f in files if f["path"] in a.original]
        missing = set(a.original) - {f["path"] for f in orig}
        if missing:
            Fail("not in the provenance: " + ", ".join(sorted(missing)))
    else:
        orig = [f for f in files if "archive/" not in f["path"][:8] and
                pathlib.PurePosixPath(f["path"]).suffix.lower() in MODEL_EXT | {".bin", ".png", ".jpg", ".jpeg"}]
    archives = [f for f in files if f["path"].startswith("archive/")]

    changes = list(a.change)
    norm = None
    if a.normalize_report:
        norm = json.loads(pathlib.Path(a.normalize_report).read_text())
        changes = ["%s: %s" % (norm.get("tool", "normalize_model.py"), c) for c in norm.get("changes", [])] + changes
    insp = ParseInspect(pathlib.Path(a.inspect).read_text()) if a.inspect else None

    date = prov["downloadedAt"][:10]
    authors = ", ".join(prov.get("authors") or []) or "unknown"
    urls = [f.get("url") for f in orig + archives if f.get("url")]
    lines = ["# %s" % rel, "",
             "Provenance of `%s` in this package (written by `tools/record_asset.py`)." % rel, "",
             "| Field | Value |", "|---|---|",
             "| Name | %s |" % Cell(prov.get("title") or rel.name),
             "| Package path | `%s` |" % rel,
             "| Source | %s |" % Cell(prov["source"]),
             "| Asset page | %s |" % Cell(prov.get("pageUrl") or "-"),
             "| Download URL(s) | %s |" % Cell("<br>".join(dict.fromkeys(urls)) or "-"),
             "| Author(s) | %s |" % Cell(authors),
             "| Licence | %s (%s) |" % (Cell(lic), Cell(prov["licenceUrl"])),
             "| Attribution | %s |" % Cell(prov.get("attribution") or "not required (CC0); credit given in ASSETS.md anyway"),
             "| Downloaded | %s (by %s) |" % (date, Cell(prov.get("tool", "?"))),
             "| AI-generated | %s |" % a.ai_generated]
    for f in archives:
        lines.append("| Archive sha256 | `%s` %s |" % (f["sha256"], Cell(pathlib.PurePosixPath(f["path"]).name)))
    for f in orig:
        lines.append("| Original sha256 | `%s` %s%s |" % (f["sha256"], Cell(f["path"]),
                     " (md5 verified against the API)" if f.get("md5Verified") else ""))
    if prov.get("polycount"):
        lines.append("| Triangles (source) | %s |" % prov["polycount"])
    if insp:
        for k in ("format", "generator", "extensionsUsed", "triangles", "size", "up axis", "units"):
            if k in insp:
                lines.append("| %s (this file) | %s |" % (k[0].upper() + k[1:], Cell(insp[k])))
        if insp["materials"]:
            lines.append("| Material slots | %s |" % Cell("; ".join("[%s] %s" % m for m in insp["materials"])))
        if insp["maps"]:
            lines.append("| Texture maps | %s |" % Cell("; ".join(insp["maps"])))
    lines += ["", "## Changes to the file", ""]
    lines += ["- " + c for c in changes] or ["- none: the file is byte-identical to the original"]
    if insp and insp["warnings"]:
        lines += ["", "## --inspect-model warnings", ""] + ["- " + w for w in insp["warnings"]]
    if a.note:
        lines += ["", "## Notes", ""] + ["- " + n for n in a.note]
    sidecar = pkg / (str(rel) + ".asset.md")
    sidecar.write_text("\n".join(lines) + "\n")
    print("wrote " + str(sidecar))

    # ASSETS.md row
    src_url = prov.get("pageUrl") or (urls[0] if urls else "-")
    if urls and urls[0] != src_url:
        src_url += " (file: %s)" % urls[0]
    licence_cell = "%s, %s" % (lic, prov["licenceUrl"])
    if ccby:
        licence_cell += "; attribution: %s" % prov["attribution"]
    sha = (orig[0]["sha256"] if orig else (archives[0]["sha256"] if archives else "-"))
    change_cell = "; ".join(changes) if changes else "none"
    row = "| `%s` | %s | %s | %s | %s | `%s` | %s |" % (
        Cell(rel), Cell(src_url), Cell(authors), Cell(licence_cell), date, sha, Cell(change_cell))
    text = assets.read_text().rstrip("\n").split("\n")
    key = "| `%s` |" % rel
    for i, l in enumerate(text):
        if l.startswith(key):
            text[i] = row
            break
    else:
        if HEADER not in text:
            text += ["", HEADER, RULE]
        last = max(i for i, l in enumerate(text) if l.startswith("|"))
        text.insert(last + 1, row)
    assets.write_text("\n".join(text) + "\n")
    print("updated " + str(assets))


if __name__ == "__main__":
    main()
