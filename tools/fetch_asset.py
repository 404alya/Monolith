#!/usr/bin/env python3
# fetch_asset.py -- licence-safe downloads of models and textures for monolith map packages.
# Run with `python3 -I tools/fetch_asset.py <source> <command> ...` (stdlib only); sources: polyhaven,
# ambientcg, kenney, polypizza, polyfork, huggingface (the embedding model only). Every download goes into a
# new, empty directory outside the repo, archives are audited, nothing downloaded is ever executed, and
# provenance.json records source, licence and hashes. `get --expect-*` refuses a file whose hash differs.
# `<source> catalog` writes the model library's interchange file (.jsonl, docs/model-library.md §8).
# Requests to one host are at least 1 s apart; HTTP 429 is answered with Retry-After and back-off.
# Reference: docs/code/fetch_asset.md

import argparse
import datetime
import email.utils
import fnmatch
import hashlib
import json
import os
import pathlib
import re
import shutil
import stat
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile

TOOL_VERSION = "1"
USER_AGENT = ("monolith-map-designer/2 (AI-assisted level editor for raylib + R3D; "
              "tools/fetch_asset.py " + TOOL_VERSION + ")")
MAX_FILE = 200 * 1024 * 1024            # per downloaded file (a Kenney pack zip can be ~100 MB)
MAX_ENTRY = 64 * 1024 * 1024            # per extracted archive entry
MAX_ENTRIES = 20000
MAX_HF_FILE = 1024 * 1024 * 1024       # huggingface only: the embedding model GGUF is ~640 MB
PACE = 1.0                              # seconds between two requests to one host
HOST_PACE = {"api.poly.pizza": 2.0}     # hosts that get more room
RETRIES_429 = 3                         # tries after the first HTTP 429, each wait doubled
REPO = pathlib.Path(__file__).resolve().parent.parent

HOSTS = {
    "polyhaven": {"api.polyhaven.com", "dl.polyhaven.org"},
    "ambientcg": {"ambientcg.com", "acg-download.struffelproductions.com"},
    "polypizza": {"api.poly.pizza", "static.poly.pizza"},
    "polyfork": {"polyfork.dev"},
    # resolve/ answers with a relative redirect (small files) or one to the xet CDN (LFS files), checked
    # by hand with curl -sIL on 2026-11-20
    "huggingface": {"huggingface.co", "us.aws.cdn.hf.co"},
}
MODEL_EXT = {".glb", ".gltf", ".bin", ".obj", ".mtl", ".fbx", ".dae"}
IMAGE_EXT = {".png", ".jpg", ".jpeg"}
LICENCE_NAMES = {"license.txt", "licence.txt", "license.md", "license", "copying.txt"}

CC0_URL = "https://creativecommons.org/publicdomain/zero/1.0/"


class Fail(Exception):
    pass


class HttpFail(Fail):
    def __init__(self, code, msg):
        super().__init__(msg)
        self.code = code


# ----------------------------------------------------------------------------- HTTP

class _Redirects(urllib.request.HTTPRedirectHandler):
    def __init__(self, hosts):
        super().__init__()
        self.hosts = hosts

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        CheckUrl(newurl, self.hosts)
        new = super().redirect_request(req, fp, code, msg, headers, newurl)
        if new is not None:
            # Never forward the Poly Pizza key to another host (urllib keeps headers on redirect)
            if urllib.parse.urlsplit(newurl).hostname != "api.poly.pizza":
                for h in list(new.headers):
                    if h.lower() == "x-auth-token":
                        del new.headers[h]
        return new


def CheckUrl(url, hosts):
    u = urllib.parse.urlsplit(url)
    if u.scheme != "https":
        raise Fail("refusing non-https URL: " + url)
    if u.hostname not in hosts:
        raise Fail("refusing URL outside this source's hosts (%s): %s" % (", ".join(sorted(hosts)), url))


_LAST = {}                              # host -> time.monotonic() of its last request
REQUESTS = {}                           # host -> number of requests, for the catalog summaries


def Pace(url):
    host = urllib.parse.urlsplit(url).hostname
    pace = HOST_PACE.get(host, PACE)
    wait = _LAST.get(host, -pace) + pace - time.monotonic()
    if wait > 0:
        time.sleep(wait)
    _LAST[host] = time.monotonic()
    REQUESTS[host] = REQUESTS.get(host, 0) + 1


def RetryAfter(headers):
    """Seconds to wait from a Retry-After header (seconds or an HTTP date); 60 when missing or odd."""
    v = (headers.get("Retry-After") or "").strip() if headers else ""
    if v.isdigit():
        return min(int(v), 3600)
    try:
        when = email.utils.parsedate_to_datetime(v)
        return min(max(0, int((when - datetime.datetime.now(datetime.timezone.utc)).total_seconds())), 3600)
    except (TypeError, ValueError):
        return 60


def Open(url, hosts, headers=None):
    CheckUrl(url, hosts)
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, **(headers or {})})
    opener = urllib.request.build_opener(_Redirects(hosts))
    for attempt in range(RETRIES_429 + 1):
        Pace(url)
        try:
            return opener.open(req, timeout=60)
        except urllib.error.HTTPError as e:
            if e.code != 429:
                raise HttpFail(e.code, "HTTP %d for %s" % (e.code, Redact(url)))
            if attempt == RETRIES_429:
                raise Fail("HTTP 429 (too many requests) for %s %d times: giving up; try again later"
                           % (Redact(url), attempt + 1))
            wait = RetryAfter(e.headers) * (2 ** attempt)
            print("fetch_asset: HTTP 429 from %s: waiting %d s (try %d of %d)" %
                  (urllib.parse.urlsplit(url).hostname, wait, attempt + 2, RETRIES_429 + 1), file=sys.stderr)
            time.sleep(wait)
        except urllib.error.URLError as e:
            raise Fail("cannot reach %s: %s" % (Redact(url), e.reason))


def Redact(url):
    return url  # no source puts a key into a URL; kept as the single place to change if one does


def GetJson(url, hosts, headers=None):
    with Open(url, hosts, headers) as r:
        data = r.read(16 * 1024 * 1024 + 1)
    if len(data) > 16 * 1024 * 1024:
        raise Fail("API response too large: " + url)
    try:
        return json.loads(data.decode("utf-8"))
    except ValueError:
        raise Fail("API response is not JSON: " + url)


def Download(url, hosts, dest, limit=MAX_FILE, final_suffix=None):
    """Stream url into dest (must not exist). Returns (size, md5, sha256, final url).
    final_suffix: refuse (before writing anything) when the URL after redirects has another ending."""
    if dest.exists():
        raise Fail("refusing to overwrite " + str(dest))
    md5, sha = hashlib.md5(), hashlib.sha256()
    size = 0
    with Open(url, hosts) as r:
        final = r.geturl()
        CheckUrl(final, hosts)
        if final_suffix and not urllib.parse.urlsplit(final).path.lower().endswith(final_suffix):
            raise Fail("%s ended at %s, which is not a %s file" % (url, final, final_suffix))
        length = r.headers.get("Content-Length")
        if length and int(length) > limit:
            raise Fail("%s is %s bytes, over the %d byte limit" % (url, length, limit))
        dest.parent.mkdir(parents=True, exist_ok=True)
        with open(dest, "xb") as f:
            while True:
                chunk = r.read(1 << 20)
                if not chunk:
                    break
                size += len(chunk)
                if size > limit:
                    raise Fail("%s exceeds the %d byte limit" % (url, limit))
                md5.update(chunk)
                sha.update(chunk)
                f.write(chunk)
    return size, md5.hexdigest(), sha.hexdigest(), final


def Sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# ----------------------------------------------------------------------------- paths

def SafeRel(name):
    """A relative path from an API or archive, checked: no absolute, '..', backslash, odd chars."""
    if not name or "\\" in name or name.startswith("/") or "\x00" in name:
        return None
    parts = name.split("/")
    if any(p in ("", ".", "..") for p in parts):
        return None
    if re.match(r"^[A-Za-z]:", name):
        return None
    if not all(re.fullmatch(r"[A-Za-z0-9 _.()+,@&'-]+", p) for p in parts):
        return None
    return pathlib.PurePosixPath(*parts)


def OutDir(arg, source, ident):
    if arg:
        out = pathlib.Path(arg).expanduser().resolve()
    else:
        base = os.environ.get("MONOLITH_DOWNLOADS") or "~/.cache/monolith/downloads"
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        out = pathlib.Path(base).expanduser().resolve() / ("%s-%s-%s" % (source, ident, stamp))
    if out == REPO or REPO in out.parents:
        raise Fail("download directories must be outside the repo (%s); copy the checked files "
                   "into maps/<name>/ afterwards" % REPO)
    if out.exists():
        if not out.is_dir() or any(out.iterdir()):
            raise Fail("%s exists and is not an empty directory: every download needs a new one" % out)
    else:
        out.mkdir(parents=True)
    return out


def Now():
    return datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0).isoformat()


def WriteProvenance(out, prov):
    prov = {"tool": "tools/fetch_asset.py", "toolVersion": TOOL_VERSION,
            "downloadedAt": Now(), **prov}
    path = out / "provenance.json"
    with open(path, "x") as f:
        json.dump(prov, f, indent=2)
        f.write("\n")
    print("provenance: " + str(path))
    print("files in " + str(out) + ":")
    for fe in prov.get("files", []):
        print("  %-60s %10d  sha256 %s" % (fe["path"], fe["size"], fe["sha256"]))


# ----------------------------------------------------------------------------- expected hashes

def Trash(paths):
    paths = [str(x) for x in paths]
    if not paths:
        return
    if not shutil.which("gio"):
        raise Fail("gio is missing: install glib2 (gio trash) so refused or used downloads can be moved to the trash")
    r = subprocess.run(["gio", "trash", "--"] + paths, capture_output=True, text=True)
    if r.returncode != 0:
        raise Fail("gio trash failed: " + r.stderr.strip())


def AddExpectArgs(g, md5=True, what="the model file"):
    if md5:
        g.add_argument("--expect-md5", metavar="HEX", help="refuse unless the md5 of %s is this" % what)
    g.add_argument("--expect-sha256", metavar="HEX", help="refuse unless the sha256 of %s is this" % what)


def Expected(a):
    """[(kind, lower-case hex)] from --expect-md5 / --expect-sha256, checked for shape."""
    out = []
    for kind, n in (("md5", 32), ("sha256", 64)):
        v = getattr(a, "expect_" + kind, None)
        if v is None:
            continue
        v = v.strip().lower()
        if not re.fullmatch(r"[0-9a-f]{%d}" % n, v):
            raise Fail("--expect-%s needs %d hex digits" % (kind, n))
        out.append((kind, v))
    return out


def Mismatches(expected, rec):
    """The (kind, expected, got) pairs where rec (a files[] entry with md5/sha256) differs."""
    return [(k, v, rec.get(k)) for k, v in expected if rec.get(k) != v]


def Discard(paths):
    """Move refused files out of the way: to the trash, or, where gio can't trash (it refuses /tmp, a system
    internal mount), delete them, which CLAUDE.md allows only for scratch files under /tmp. Returns how."""
    paths = [pathlib.Path(x) for x in paths]
    try:
        Trash(paths)
        return "moved to the trash (gio trash)"
    except Fail:
        tmp = pathlib.Path("/tmp").resolve()
        if not paths or not all(tmp in p.resolve().parents for p in paths):
            raise
        for p in paths:
            if p.is_dir() and not p.is_symlink():
                shutil.rmtree(p)
            else:
                p.unlink()
        return "deleted (scratch under /tmp, which gio can't trash)"


def Refuse(out, what, bad, files=None, note="REFUSED.txt"):
    """Move the downloaded files (default: everything in out) out of the way (Discard), leave `note` in out
    naming both hashes, and fail with exit 1. bad: [(kind, expected, got)]. The note is written even when the
    files can't be moved, so a refused folder is always marked."""
    files = sorted(out.iterdir()) if files is None else files
    lines = ["refused: %s" % what]
    for kind, want, got in bad:
        lines.append("%s expected %s" % (kind, want))
        lines.append("%s got      %s" % (kind, got))
    try:
        how = Discard(files)
    except Fail as e:
        lines.append("the downloaded files could NOT be moved (%s); none of them may be used" % e)
        (out / note).write_text("\n".join(lines) + "\n")
        raise
    lines.append("the downloaded files were %s; nothing here may be used" % how)
    (out / note).write_text("\n".join(lines) + "\n")
    raise Fail("hash mismatch, refused: %s (%s); see %s" %
               (what, "; ".join("%s expected %s, got %s" % b for b in bad), out / note))


# ----------------------------------------------------------------------------- catalog files (.jsonl)

# docs/model-library.md §8: the §3 columns a harvest knows, in §3 order, then two fields that are not stored
CATALOG_FIELDS = ["id", "source", "name", "collection", "page_url", "file_url", "path_in_zip", "file_hash",
                  "hash_kind", "zip_sha256", "licence", "licence_url", "attribution", "authors", "rigged", "ik",
                  "tags", "tris", "sx", "sy", "sz", "slots", "flat_shaded", "textured", "factor_colours_only",
                  "preview", "source_tags"]
CC_BY_4_URL = "https://creativecommons.org/licenses/by/4.0/"


def CatalogDir(arg):
    out = pathlib.Path(arg).expanduser().resolve()
    if out == REPO or REPO in out.parents:
        raise Fail("catalog files are scratch: write them outside the repo (e.g. /tmp/monolith-library), not %s"
                   % out)
    out.mkdir(parents=True, exist_ok=True)
    return out


def TagWords(*texts):
    """Lower-case words of the given texts, unique, sorted, space-separated."""
    words = set()
    for t in texts:
        words.update(re.findall(r"[a-z0-9]+", (t or "").lower()))
    return " ".join(sorted(words))


def Size(v):
    return None if v is None else round(float(v), 4)


def ReadJsonl(path):
    rows = []
    if path.exists():
        with open(path, encoding="utf-8") as f:
            for n, line in enumerate(f, 1):
                if line.strip():
                    try:
                        rows.append(json.loads(line))
                    except ValueError:
                        raise Fail("%s:%d is not JSON" % (path, n))
    return rows


def WriteJsonl(path, rows):
    """One object per line, sorted by id, keys in CATALOG_FIELDS order, no timestamps: an unchanged source
    gives a byte-identical file."""
    seen = set()
    lines = []
    for row in sorted(rows, key=lambda r: r["id"]):
        if row["id"] in seen:
            raise Fail("duplicate id in the catalog: " + row["id"])
        seen.add(row["id"])
        extra = set(row) - set(CATALOG_FIELDS)
        if extra:
            raise Fail("unknown catalog keys %s in %s" % (sorted(extra), row["id"]))
        lines.append(json.dumps({k: row.get(k) for k in CATALOG_FIELDS}, ensure_ascii=False,
                                separators=(",", ":")))
    tmp = path.with_name(path.name + ".tmp")
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write("".join(l + "\n" for l in lines))
    os.replace(tmp, path)
    print("wrote %s: %d rows" % (path, len(lines)))


def RequestSummary(t0):
    took = time.monotonic() - t0
    print("requests: %s; %.0f s" % (", ".join("%s %d" % kv for kv in sorted(REQUESTS.items())) or "none", took))


# ----------------------------------------------------------------------------- archives

def AuditZip(zpath):
    """Open a zip and check every entry; returns (zipfile, list of (info, safe relpath))."""
    try:
        z = zipfile.ZipFile(zpath)
    except zipfile.BadZipFile:
        raise Fail("not a zip file: %s" % zpath)
    infos = z.infolist()
    if len(infos) > MAX_ENTRIES:
        raise Fail("archive has %d entries (limit %d)" % (len(infos), MAX_ENTRIES))
    entries, bad = [], []
    for info in infos:
        mode = (info.external_attr >> 16) & 0xFFFF
        if stat.S_ISLNK(mode):
            bad.append(info.filename + " (symlink)")
            continue
        if info.is_dir():
            continue
        rel = SafeRel(info.filename)
        if rel is None:
            bad.append(info.filename + " (unsafe name)")
            continue
        if info.file_size > MAX_ENTRY:
            bad.append("%s (%d bytes)" % (info.filename, info.file_size))
            continue
        entries.append((info, rel))
    if bad:
        print("audit: skipped %d unsafe entries:" % len(bad))
        for b in bad[:20]:
            print("  " + b)
    print("audit: %d entries OK" % len(entries))
    return z, entries


def Extract(z, info, rel, dest_root, limit=MAX_ENTRY):
    dest = dest_root / pathlib.Path(*rel.parts)
    if dest.exists():
        raise Fail("refusing to overwrite " + str(dest))
    dest.parent.mkdir(parents=True, exist_ok=True)
    size = 0
    with z.open(info) as src, open(dest, "xb") as f:
        while True:
            chunk = src.read(1 << 20)
            if not chunk:
                break
            size += len(chunk)
            if size > limit:
                raise Fail("entry grew past its declared size: " + info.filename)
            f.write(chunk)
    return dest


# ----------------------------------------------------------------------------- Poly Haven

PH_API = "https://api.polyhaven.com"


def PolyHavenSearch(a):
    url = PH_API + "/assets?type=models"
    if a.category:
        url += "&categories=" + urllib.parse.quote(a.category)
    data = GetJson(url, HOSTS["polyhaven"])
    words = [w.lower() for w in (a.text or [])]
    rows = []
    for slug, info in data.items():
        hay = " ".join([slug, info.get("name", "")] + info.get("tags", []) +
                       info.get("categories", [])).lower()
        if any(w not in hay for w in words):
            continue
        tris = info.get("polycount") or 0
        if a.max_tris and tris > a.max_tris:
            continue
        dims = info.get("dimensions") or [0, 0, 0]
        rows.append((tris, slug, info.get("name", ""), dims, ", ".join(info.get("authors", {}))))
    rows.sort()
    print("Poly Haven models (CC0, https://polyhaven.com/license): %d match" % len(rows))
    print("%-28s %9s  %-24s %s" % ("slug", "tris", "size m (x, depth, height)", "author"))
    for tris, slug, name, dims, authors in rows[:a.limit]:
        size = "%.2f %.2f %.2f" % tuple(d / 1000.0 for d in dims[:3])
        print("%-28s %9d  %-24s %s" % (slug, tris, size, authors))


def PolyHavenGet(a):
    slug = a.slug
    if not re.fullmatch(r"[A-Za-z0-9_-]+", slug):
        raise Fail("bad slug: " + slug)
    expected = Expected(a)
    hosts = HOSTS["polyhaven"]
    info = GetJson(PH_API + "/info/" + slug, hosts)
    if info.get("type") != 2:
        raise Fail("%s is not a model on Poly Haven" % slug)
    files = GetJson(PH_API + "/files/" + slug, hosts)
    gl = files.get("gltf", {}).get(a.res, {}).get("gltf")
    if not gl:
        raise Fail("no glTF at %s for %s (available: %s)" %
                   (a.res, slug, ", ".join(sorted(files.get("gltf", {})))))
    out = OutDir(a.out, "polyhaven", slug)
    todo = [(SafeRel(pathlib.PurePosixPath(urllib.parse.urlsplit(gl["url"]).path).name), gl)]
    for name, f in sorted(gl.get("include", {}).items()):
        todo.append((SafeRel(name), f))
    recs = []
    for rel, f in todo:
        if rel is None:
            raise Fail("unsafe file name in the API's include map")
        if rel.suffix.lower() not in MODEL_EXT | IMAGE_EXT:
            print("skip (not a model/texture file): " + str(rel))
            continue
        size, md5, sha, final = Download(f["url"], hosts, out / pathlib.Path(*rel.parts))
        rec = {"path": str(rel), "url": f["url"], "size": size,
               "md5": md5, "md5Verified": bool(f.get("md5")), "sha256": sha}
        if not recs and expected:       # the .gltf comes first: check it before fetching the rest
            bad = Mismatches(expected, rec)
            if bad:
                Refuse(out, "polyhaven %s %s" % (slug, rel), bad)
            print("expected hash matches: %s" % rel)
        if f.get("md5") and md5 != f["md5"]:
            raise Fail("md5 mismatch for %s: API %s, got %s" % (rel, f["md5"], md5))
        recs.append(rec)
    dims = info.get("dimensions") or []
    WriteProvenance(out, {
        "source": "Poly Haven", "assetId": slug, "title": info.get("name"),
        "pageUrl": "https://polyhaven.com/a/" + slug,
        "apiUrls": [PH_API + "/info/" + slug, PH_API + "/files/" + slug],
        "authors": list(info.get("authors", {})),
        "licence": "CC0 1.0", "licenceUrl": "https://polyhaven.com/license",
        "attribution": None, "resolution": a.res, "format": "glTF",
        "polycount": info.get("polycount"),
        "dimensionsM": [d / 1000.0 for d in dims[:3]] if dims else None,  # Blender order: x, y (depth), z (up)
        "files": recs})


def PolyHavenCatalog(a):
    """One row per model: the list (/assets?t=models already carries every /info field but a download
    counter) and /files/<slug> for the 1k glTF and its md5. Writes <out>/polyhaven.jsonl."""
    t0 = time.monotonic()
    out = CatalogDir(a.out)
    hosts = HOSTS["polyhaven"]
    data = GetJson(PH_API + "/assets?t=models", hosts)
    slugs = sorted(k for k, v in data.items() if v.get("type") == 2)
    if a.limit:
        slugs = slugs[:a.limit]
    rows, skipped = [], []
    for n, slug in enumerate(slugs, 1):
        info = data[slug]
        if not re.fullmatch(r"[A-Za-z0-9_-]+", slug):
            skipped.append((slug, "odd slug"))
            continue
        files = GetJson(PH_API + "/files/" + slug, hosts)
        gl = files.get("gltf", {}).get("1k", {}).get("gltf") or {}
        url, md5 = gl.get("url"), (gl.get("md5") or "").lower()
        if not url:
            skipped.append((slug, "no 1k glTF"))
            continue
        CheckUrl(url, hosts)
        if not re.fullmatch(r"[0-9a-f]{32}", md5):
            skipped.append((slug, "the API gives no md5 for the 1k glTF"))
            continue
        dims = info.get("dimensions") or []
        # Poly Haven's dimensions are Blender's x, y (depth), z (up) in mm; the catalog is glTF's x, y (up), z
        sx, sy, sz = (Size(dims[0] / 1000.0), Size(dims[2] / 1000.0), Size(dims[1] / 1000.0)) \
            if len(dims) == 3 else (None, None, None)
        tags, cats = list(info.get("tags") or []), list(info.get("categories") or [])
        rows.append({
            "id": "polyhaven-" + slug, "source": "polyhaven", "name": info.get("name") or slug,
            "collection": None, "page_url": "https://polyhaven.com/a/" + slug, "file_url": url,
            "path_in_zip": None, "file_hash": md5, "hash_kind": "md5", "zip_sha256": None,
            "licence": "CC0 1.0", "licence_url": "https://polyhaven.com/license", "attribution": None,
            "authors": "; ".join(sorted(info.get("authors") or {})), "rigged": 0, "ik": 0,
            "tags": TagWords(*(tags + cats)), "tris": info.get("polycount") or None,
            "sx": sx, "sy": sy, "sz": sz, "slots": None, "flat_shaded": 0, "textured": 1,
            "factor_colours_only": 0, "preview": info.get("thumbnail_url"), "source_tags": tags + cats})
        if n % 50 == 0:
            print("  %d / %d" % (n, len(slugs)), file=sys.stderr)
    for slug, why in skipped:
        print("skipped %s: %s" % (slug, why))
    WriteJsonl(out / "polyhaven.jsonl", rows)
    RequestSummary(t0)


# ----------------------------------------------------------------------------- ambientCG

ACG_API = "https://ambientcg.com/api/v3/assets"


def AmbientSearch(a):
    q = urllib.parse.urlencode({"q": " ".join(a.text), "type": "material", "limit": a.limit,
                                "include": "title,url,tags,dimensions,downloads"})
    data = GetJson(ACG_API + "?" + q, HOSTS["ambientcg"])
    print("ambientCG materials (CC0, https://docs.ambientcg.com/license/): %s match" %
          data.get("totalResults"))
    for asset in data.get("assets", []):
        d = asset.get("dimensions") or {}
        res = [x.get("attributes") for x in asset.get("downloads", [])
               if x.get("attributes", "").endswith("-JPG")]
        print("%-16s %-24s tile %.2f x %.2f m  %s" % (
            asset["id"], asset.get("title", ""), d.get("width", 0) / 100.0,
            d.get("height", 0) / 100.0, " ".join(res)))


def AmbientGet(a):
    ident = a.id
    if not re.fullmatch(r"[A-Za-z0-9_]+", ident):
        raise Fail("bad asset id: " + ident)
    hosts = HOSTS["ambientcg"]
    q = urllib.parse.urlencode({"id": ident, "include": "title,url,tags,dimensions,downloads"})
    data = GetJson(ACG_API + "?" + q, hosts)
    assets = [x for x in data.get("assets", []) if x.get("id", "").lower() == ident.lower()]
    if not assets:
        raise Fail("no ambientCG asset " + ident)
    asset = assets[0]
    want = a.res.upper() + "-JPG"
    dl = [x for x in asset.get("downloads", []) if x.get("attributes") == want]
    if not dl:
        raise Fail("no %s download (have: %s)" % (want, ", ".join(
            x.get("attributes", "?") for x in asset.get("downloads", []))))
    dl = dl[0]
    out = OutDir(a.out, "ambientcg", asset["id"])
    # the zip goes into a sub-folder so the extracted images sit alone in out/
    zname = SafeRel(pathlib.PurePosixPath(urllib.parse.urlsplit(dl["url"]).query.split("=")[-1]).name) \
        or pathlib.PurePosixPath("download.zip")
    zdir = out / "archive"
    size, md5, sha, final = Download(dl["url"], hosts, zdir / zname.name)
    z, entries = AuditZip(zdir / zname.name)
    recs = [{"path": "archive/" + zname.name, "url": dl["url"], "finalUrl": final,
             "size": size, "md5": md5, "sha256": sha}]
    for info, rel in entries:
        if len(rel.parts) != 1 or rel.suffix.lower() not in IMAGE_EXT:
            print("skip: " + info.filename)
            continue
        dest = Extract(z, info, rel, out)
        recs.append({"path": rel.name, "fromArchive": "archive/" + zname.name,
                     "size": dest.stat().st_size, "sha256": Sha256(dest)})
    d = asset.get("dimensions") or {}
    WriteProvenance(out, {
        "source": "ambientCG", "assetId": asset["id"], "title": asset.get("title"),
        "pageUrl": asset.get("url"), "apiUrls": [ACG_API + "?" + q],
        "authors": ["ambientCG (Lennart Demes)"],
        "licence": "CC0 1.0", "licenceUrl": "https://docs.ambientcg.com/license/",
        "attribution": None, "resolution": want,
        "tileSizeM": [d.get("width", 0) / 100.0, d.get("height", 0) / 100.0],
        "files": recs})
    print("maps: *_Color = albedo, *_NormalGL = normal (OpenGL), *_Roughness, *_AmbientOcclusion; "
          "build an ORM with the occlusion in R and roughness in G if you need both")


# ----------------------------------------------------------------------------- Kenney

# No agent fetches from kenney.nl: on 2026-11-20 its robots.txt disallowed Claude's agents site-wide and the
# pack pages gave only browser-signed zip links. The developer downloads pack zips (or the All-in-1 bundle) in
# a browser into the cache folder; this tool only reads them there and never moves or changes them.
KENNEY_CACHE = "~/.cache/monolith/kenney"
KENNEY_MAX_ENTRIES = 200000             # the All-in-1 bundle is one zip of every pack
SLUG = r"[a-z0-9]+(?:-[a-z0-9]+)*"


def KenneyCacheDir():
    return pathlib.Path(os.environ.get("MONOLITH_KENNEY_ZIPS") or KENNEY_CACHE).expanduser().resolve()


def KenneyPage(slug):
    return "https://kenney.nl/assets/" + slug          # a string for the record only, never fetched


def KenneyIndex(cache):
    """{zip path: sha256} of every *.zip in the cache folder (not below it). The hashes are kept in
    <cache>/.sha256-index keyed by name, size and mtime, so an unchanged zip isn't hashed again."""
    if not cache.is_dir():
        return {}
    ipath = cache / ".sha256-index"
    try:
        old = json.loads(ipath.read_text("utf-8")) if ipath.is_file() else {}
        if not isinstance(old, dict):
            old = {}
    except ValueError:
        old = {}
    new, out = {}, {}
    for z in sorted(p for p in cache.iterdir() if p.suffix.lower() == ".zip" and p.is_file()):
        st = z.stat()
        e = old.get(z.name)
        if not (isinstance(e, dict) and e.get("size") == st.st_size and e.get("mtime_ns") == st.st_mtime_ns
                and re.fullmatch(r"[0-9a-f]{64}", str(e.get("sha256")))):
            e = {"size": st.st_size, "mtime_ns": st.st_mtime_ns, "sha256": Sha256(z)}
        new[z.name] = e
        out[z] = e["sha256"]
    if new != old:
        tmp = cache / ".sha256-index.tmp"
        tmp.write_text(json.dumps(new, indent=1, sort_keys=True) + "\n", "utf-8")
        os.replace(tmp, ipath)
    return out


def SlugOf(name):
    """A pack slug from a zip or folder name: 'kenney_car-kit.zip', 'kenney_car-kit_2.0.zip', 'Car Kit',
    'Car Kit (1.2)'; None when nothing usable is left."""
    s = re.sub(r"\.zip$", "", name, flags=re.I)
    s = re.sub(r"^kenney[_ -]+", "", s, flags=re.I)
    s = re.sub(r"[_ ]+v?[0-9]+(\.[0-9]+)*$|\s*\([^)]*\)$", "", s)
    s = re.sub(r"[^a-z0-9]+", "-", s.lower()).strip("-")
    return s if re.fullmatch(SLUG, s) else None


def ZipOpen(zpath, quiet=True):
    """AuditZip for zips from the cache: more entries (the bundle) and nested pack zips up to MAX_FILE."""
    try:
        z = zipfile.ZipFile(zpath)
    except zipfile.BadZipFile:
        raise Fail("not a zip file: %s" % zpath)
    infos = z.infolist()
    if len(infos) > KENNEY_MAX_ENTRIES:
        raise Fail("%s has %d entries (limit %d)" % (zpath, len(infos), KENNEY_MAX_ENTRIES))
    entries, bad = [], 0
    for info in infos:
        if stat.S_ISLNK((info.external_attr >> 16) & 0xFFFF) or info.is_dir():
            bad += not info.is_dir()
            continue
        rel = SafeRel(info.filename)
        limit = MAX_FILE if info.filename.lower().endswith(".zip") else MAX_ENTRY
        if rel is None or info.file_size > limit:
            bad += 1
            continue
        entries.append((info, rel))
    if bad and not quiet:
        print("audit: %s: skipped %d unsafe entries" % (zpath.name, bad))
    return z, entries


def LicenceIsCc0(z, info):
    with z.open(info) as f:
        text = f.read(MAX_ENTRY).decode("utf-8", "replace")
    return "creativecommons.org/publicdomain/zero" in text or "CC0" in text


def KenneyPacks(zpath):
    """The packs in one cached zip: [(slug, nested zip entry or None, prefix)], plus [problems].
    A pack zip has License.txt at its top (slug from the zip's name). A bundle holds pack zips
    (kenney_<pack>.zip, slug from that name) or pack folders (a folder with License.txt, slug from the folder's
    name); bundle packs without .glb/.gltf models (2D, audio) are left out."""
    z, entries = ZipOpen(zpath)
    try:
        lic = [rel for _, rel in entries if rel.name.lower() in LICENCE_NAMES]
        models = [rel for _, rel in entries if rel.suffix.lower() in (".glb", ".gltf")]
        if any(len(r.parts) == 1 for r in lic):
            slug = SlugOf(zpath.name)
            if not slug:
                return [], ["%s: a pack zip, but its name doesn't say which pack (expected kenney_<pack>.zip)"
                            % zpath.name]
            return [(slug, None, "")], []
        packs, problems = [], []
        for info, rel in entries:
            if rel.suffix.lower() == ".zip":
                slug = SlugOf(rel.name)
                if slug:
                    packs.append((slug, str(rel), ""))
                else:
                    problems.append("%s: can't tell the pack of the nested zip %s" % (zpath.name, rel))
        for r in lic:
            root = r.parent
            if not any(root in m.parents for m in models):
                continue
            slug = SlugOf(root.name)
            if slug:
                packs.append((slug, None, str(root) + "/"))
            else:
                problems.append("%s: can't tell the pack of the folder %s" % (zpath.name, root))
        if not packs and not problems:
            problems.append("%s: no License.txt at its top and no pack zips or pack folders inside: can't tell "
                            "the pack" % zpath.name)
        return sorted(packs), problems
    finally:
        z.close()


class KenneyPack:
    """One pack opened for reading: entries are (info, path relative to the pack root, path in the cached
    zip). A nested pack zip is extracted into `scratch` first (a folder the caller moves to the trash)."""

    def __init__(self, zpath, nested, prefix, scratch):
        self.zips = []
        z, entries = ZipOpen(zpath)
        self.zips.append(z)
        self.outer = ""
        if nested:
            info = next((i for i, rel in entries if str(rel) == nested), None)
            if info is None:
                raise Fail("%s: no %s inside" % (zpath, nested))
            inner = Extract(z, info, SafeRel(nested), scratch, limit=MAX_FILE)
            z, entries = ZipOpen(inner)
            self.zips.append(z)
            self.outer = nested + "!/"          # path_in_zip of a nested pack: <inner zip>!/<path in it>
        self.z = z
        self.entries = []
        for info, rel in entries:
            s = str(rel)
            if s.startswith(prefix):
                self.entries.append((info, SafeRel(s[len(prefix):]), self.outer + s))
        self.licence = next(((i, r, full) for i, r, full in self.entries
                             if r.name.lower() in LICENCE_NAMES and len(r.parts) == 1), None)

    def close(self):
        for z in self.zips:
            z.close()


def KenneyFind(cache_zips, slug):
    """The cached packs named slug: [(zip path, sha256, nested, prefix)], a pack zip before a bundle."""
    found = []
    for zp, sha in sorted(cache_zips.items()):
        for s, nested, prefix in KenneyPacks(zp)[0]:
            if s == slug:
                found.append((zp, sha, nested, prefix))
    return sorted(found, key=lambda f: (f[2] is not None or f[3] != "", str(f[0])))


def Inspect(monolith, path):
    """Measured facts from `monolith --inspect-model` (no window): tris, size, slots, textured,
    factor_colours_only, flat_shaded, rigged (any animation in the file). Returns (facts, None), or (None, error) when it can't import the file."""
    r = subprocess.run([str(monolith), "--inspect-model", str(path)], capture_output=True, text=True,
                       timeout=120, cwd=str(pathlib.Path(monolith).parent))
    t = r.stdout
    tri = re.search(r"^triangles: (\d+)$", t, re.M)
    ver = re.search(r"^vertices: (\d+)$", t, re.M)
    size = re.search(r"^size: ([-\d.]+) x ([-\d.]+) x ([-\d.]+) \(x y z\)$", t, re.M)
    mats = re.search(r"^materials: (\d+) ", t, re.M)
    texs = re.search(r"^textures: (\d+) map slot", t, re.M)
    anims = re.search(r"^animations: (\d+)", t, re.M)
    if r.returncode != 0 or not (tri and ver and size and mats and texs):
        return None, ((t.strip().splitlines() or [r.stderr.strip()]) + [""])[0]
    tris, verts = int(tri.group(1)), int(ver.group(1))
    return {"tris": tris, "sx": Size(size.group(1)), "sy": Size(size.group(2)), "sz": Size(size.group(3)),
            "slots": int(mats.group(1)), "flat_shaded": int(tris > 0 and verts / tris >= 2.5),
            "textured": int(int(texs.group(1)) > 0),
            "factor_colours_only": int(not re.search(r"^ +albedo map: ", t, re.M)),
            # rigged: the file carries animations (Kenney's characters: skinned or per-part), so it can move
            "rigged": int(bool(anims) and int(anims.group(1)) > 0)}, None


def KenneyPick(entries):
    """One entry per model, not per format: of the .glb/.gltf entries (info, rel, full), the one in the best
    format folder ("GLTF format", then "GLB format", then any other; .glb before .gltf) for each path below
    that folder. Returns [(name, info, rel, full)]: name is the file stem, or the sub-folders and stem joined
    by '-' when two sub-folders hold the same stem."""
    best = {}
    for info, rel, full in entries:
        if rel.suffix.lower() not in (".glb", ".gltf"):
            continue
        dirs = list(rel.parts[:-1])
        fmt = max((i for i, d in enumerate(dirs) if "gltf" in d.lower() or "glb" in d.lower()), default=-1)
        key = tuple(dirs[fmt + 1:]) + (rel.stem,)
        low = [d.lower() for d in dirs]
        rank = 0 if any("gltf" in d for d in low) else 1 if any("glb" in d for d in low) else 2
        cand = (rank, rel.suffix.lower() != ".glb", str(rel))
        if key not in best or cand < best[key][0]:
            best[key] = (cand, info, rel, full, key)
    picks = sorted(best.values(), key=lambda b: b[0][2])
    uses = {}
    for b in picks:
        uses[b[4][-1]] = uses.get(b[4][-1], 0) + 1
    return [(key[-1] if uses[key[-1]] == 1 else "-".join(key), info, rel, full)
            for _, info, rel, full, key in picks]


def GltfUris(z, info, rel):
    """The files a .gltf or .glb names (buffers and images, not data: URIs), as pack-relative paths; the
    file is parsed as JSON only. Fails on a name that leaves the pack."""
    with z.open(info) as f:
        data = f.read(MAX_ENTRY)
    if rel.suffix.lower() == ".glb":
        if len(data) < 20 or data[:4] != b"glTF" or data[16:20] != b"JSON":
            raise Fail("%s is not a binary glTF" % rel)
        n = int.from_bytes(data[12:16], "little")
        data = data[20:20 + n]
    try:
        doc = json.loads(data.decode("utf-8"))
    except ValueError:
        raise Fail("%s: its glTF JSON doesn't parse" % rel)
    out = []
    for key in ("buffers", "images"):
        for item in doc.get(key) or []:
            uri = item.get("uri") if isinstance(item, dict) else None
            if not isinstance(uri, str) or uri.startswith("data:"):
                continue
            parts = list(rel.parent.parts)
            for p in urllib.parse.unquote(uri).split("/"):
                if p == "..":
                    if not parts:
                        raise Fail("%s names a file outside the pack: %s" % (rel, uri))
                    parts.pop()
                elif p not in ("", "."):
                    parts.append(p)
            r = SafeRel("/".join(parts))
            if r is None:
                raise Fail("%s names an unsafe file: %s" % (rel, uri))
            out.append(r)
    return out


def KenneyList(a):
    cache = KenneyCacheDir()
    zips = KenneyIndex(cache)
    if not zips:
        print("no zips in %s: download Kenney packs there in a browser (https://kenney.nl/assets)" % cache)
        return
    for zp, sha in sorted(zips.items()):
        packs, problems = KenneyPacks(zp)
        for p in problems:
            print("  " + p)
        for slug, nested, prefix in packs:
            if a.pack and slug != a.pack:
                continue
            print("%-32s %s  %s%s" % (slug, sha[:12], zp.name, " " + (nested or prefix) if nested or prefix else ""))
            if a.pack:
                scratch = OutDir(None, "kenney", "list-" + slug)
                try:
                    pk = KenneyPack(zp, nested, prefix, scratch)
                    for name, info, rel, full in KenneyPick(pk.entries):
                        print("  %8d  %s" % (info.file_size, full))
                    pk.close()
                finally:
                    Trash([scratch])


def KenneyCatalog(a):
    """Per cached pack: audit, pick one entry per model, hash it, measure it with --inspect-model in a scratch
    download folder (moved to the trash afterwards). The cached zips are only read. Rows merge into
    <out>/kenney.jsonl, replacing the earlier rows of the same packs."""
    t0 = time.monotonic()
    out = CatalogDir(a.out)
    monolith = pathlib.Path(a.monolith).expanduser().resolve()
    if not monolith.is_file():
        raise Fail("no monolith binary at %s (build it, or pass --monolith)" % monolith)
    cache = KenneyCacheDir()
    if a.zip:
        zips = {}
        for z in a.zip:
            zp = pathlib.Path(z).expanduser().resolve()
            if not zp.is_file():
                raise Fail("no such zip: %s" % zp)
            zips[zp] = Sha256(zp)
    else:
        zips = KenneyIndex(cache)
        if not zips:
            raise Fail("no zips in %s: download Kenney packs there in a browser (https://kenney.nl/assets)" % cache)
    packs = {}
    for zp, sha in sorted(zips.items()):
        found, problems = KenneyPacks(zp)
        for p in problems:
            print("skipped: " + p)
        for slug, nested, prefix in found:
            cand = (nested is not None or prefix != "", str(zp))      # a pack zip before a bundle
            if slug in packs:
                print("note: %s is in %s and %s; using %s" % (slug, packs[slug][1].name, zp.name,
                      (packs[slug] if packs[slug][0] < cand else (cand, zp))[1].name))
                if packs[slug][0] < cand:
                    continue
            packs[slug] = (cand, zp, sha, nested, prefix)
    path = out / "kenney.jsonl"
    rows = ReadJsonl(path)
    for slug in sorted(packs):
        _, zp, sha, nested, prefix = packs[slug]
        scratch = OutDir(None, "kenney", "catalog-" + slug)
        pk = None
        try:
            pk = KenneyPack(zp, nested, prefix, scratch / "nested")
            if not pk.licence or not LicenceIsCc0(pk.z, pk.licence[0]):
                print("skipped %s: no License.txt saying CC0 at the pack's top: decide by hand" % slug)
                continue
            picks = KenneyPick(pk.entries)
            if not picks:
                print("skipped %s: no .glb/.gltf models" % slug)
                continue
            # every model and image file in the picked folders and below, so a model finds its .bin and textures
            dirs = {rel.parent for _, _, rel, _ in picks}
            for info, rel, _ in pk.entries:
                if rel.suffix.lower() in MODEL_EXT | IMAGE_EXT and any(d == rel.parent or d in rel.parents
                                                                       for d in dirs):
                    Extract(pk.z, info, rel, scratch / "extracted")
            new = []
            for name, info, rel, full in picks:
                f = scratch / "extracted" / pathlib.Path(*rel.parts)
                facts, err = Inspect(monolith, f)
                if err is not None:
                    print("%s/%s: --inspect-model failed (%s): measured facts left NULL" % (slug, rel, err))
                    facts = dict.fromkeys(("tris", "sx", "sy", "sz", "slots", "flat_shaded", "textured",
                                           "factor_colours_only"))
                    facts["rigged"] = 0
                new.append({
                    "id": "kenney-%s/%s" % (slug, name), "source": "kenney", "name": name, "collection": slug,
                    "page_url": KenneyPage(slug), "file_url": KenneyPage(slug), "path_in_zip": full,
                    "file_hash": Sha256(f), "hash_kind": "sha256", "zip_sha256": sha, "licence": "CC0 1.0",
                    "licence_url": CC0_URL, "attribution": None, "authors": "Kenney", "rigged": 0, "ik": 0,
                    "tags": TagWords(slug, name, *[d for d in rel.parts[:-1]
                                                    if "format" not in d.lower() and d.lower() != "models"]),
                    **facts, "preview": None, "source_tags": list(rel.parts)})
            rows = [r for r in rows if r.get("collection") != slug] + new
            print("%s: %d models from %s%s (zip sha256 %s)" % (slug, len(new), zp.name,
                  " " + (nested or prefix) if nested or prefix else "", sha))
        finally:
            if pk:
                pk.close()
            Trash([scratch])
    WriteJsonl(path, rows)
    RequestSummary(t0)


def KenneyGet(a):
    """One model from the local zip cache: the zip with --expect-zip-sha256, else (a newer download) a cached
    zip of the same pack, where the entry's hash decides. Extracts the entry, every file it names and the
    pack's License.txt into a new download folder with provenance.json."""
    expected = Expected(a)
    zip_want = a.expect_zip_sha256.strip().lower()
    if not re.fullmatch(r"[0-9a-f]{64}", zip_want):
        raise Fail("--expect-zip-sha256 needs 64 hex digits")
    target, slug, name = a.target, a.pack, None
    m = re.fullmatch(r"kenney-(%s)/([^/]+)" % SLUG, target)
    if m:                                    # a catalog id: kenney-<pack>/<name>
        slug, name = slug or m.group(1), m.group(2)
        if slug != m.group(1):
            raise Fail("--pack %s doesn't match the id %s" % (slug, target))
    if slug and not re.fullmatch(SLUG, slug):
        raise Fail("bad pack name: " + slug)
    cache = KenneyCacheDir()
    zips = KenneyIndex(cache)
    hits = [zp for zp, sha in sorted(zips.items()) if sha == zip_want]
    where = []
    for zp in hits:
        for s, nested, prefix in KenneyPacks(zp)[0]:
            if slug is None or s == slug:
                where.append((zp, zip_want, nested, prefix, s))
    if hits and not where:
        raise Fail("the cached zip %s has no pack %s" % (hits[0].name, slug))
    if hits and len({w[4] for w in where}) > 1:
        raise Fail("%s holds several packs: pass --pack" % hits[0].name)
    changed = not where
    if changed:
        if not slug:
            raise Fail("no cached zip has sha256 %s: pass --pack so a newer zip of the pack can be checked" % zip_want)
        where = [f + (slug,) for f in KenneyFind(zips, slug)]
        if not where:
            raise Fail("Kenney pack %s isn't in the cache %s: download it in a browser from %s and save the zip "
                       "there" % (slug, cache, KenneyPage(slug)))
    zp, sha, nested, prefix, slug = where[0]
    out = OutDir(a.out, "kenney", slug)
    scratch = OutDir(None, "kenney", "nested-" + slug) if nested else None
    pk = KenneyPack(zp, nested, prefix, scratch) if nested else KenneyPack(zp, None, prefix, None)
    try:
        if name is not None:
            sel = [(i, r, f) for n, i, r, f in KenneyPick(pk.entries) if n == name]
        else:
            sel = [(i, r, f) for i, r, f in pk.entries if f == target] or \
                  [(i, r, f) for i, r, f in pk.entries if str(r) == target or target.endswith("/" + str(r))]
        sel = [s for s in sel if s[1].suffix.lower() in (".glb", ".gltf")]
        if len(sel) != 1:
            raise Fail("%s matches %d models in %s %s (see `kenney list %s`)" % (target, len(sel), slug,
                       zp.name, slug))
        info, rel, full = sel[0]
        if not pk.licence or not LicenceIsCc0(pk.z, pk.licence[0]):
            raise Fail("%s: no License.txt saying CC0 at the pack's top: decide by hand" % slug)
        byrel = {str(r): (i, r, f) for i, r, f in pk.entries}
        want = [(info, rel, full), pk.licence]
        for u in GltfUris(pk.z, info, rel):
            if str(u) not in byrel:
                raise Fail("%s names %s, which isn't in the pack" % (full, u))
            if byrel[str(u)] not in want:
                want.append(byrel[str(u)])
        recs = [{"path": zp.name, "localFile": str(zp), "nested": nested, "sha256": sha,
                 "size": zp.stat().st_size}]
        for i, r, f in want:
            dest = Extract(pk.z, i, r, out / "extracted")
            recs.append({"path": "extracted/" + str(r), "fromArchive": f, "size": dest.stat().st_size,
                         "sha256": Sha256(dest)})
    finally:
        pk.close()
        if scratch:
            Trash([scratch])
    fe = recs[1]
    if "md5" in dict(expected):
        fe["md5"] = hashlib.md5((out / fe["path"]).read_bytes()).hexdigest()
    bad = Mismatches(expected, fe)
    if bad:
        Refuse(out, "kenney %s/%s" % (slug, rel), ([("zip sha256", zip_want, sha)] if changed else []) + bad)
    if changed:
        print("no cached zip has sha256 %s; %s (sha256 %s) holds the same %s: using it; the catalog's zip_sha256 "
              "is out of date" % (zip_want, zp.name, sha, rel))
    print("expected hash matches: %s" % rel)
    WriteProvenance(out, {
        "source": "Kenney", "assetId": "kenney-%s/%s" % (slug, name or rel.stem), "title": rel.stem,
        "pageUrl": KenneyPage(slug), "apiUrls": [], "authors": ["Kenney (www.kenney.nl)"],
        "licence": "CC0 1.0", "licenceUrl": CC0_URL, "licenceFile": "extracted/" + str(pk.licence[1]),
        "attribution": None, "zipSha256": sha, "pathInZip": full, "files": recs})


# ----------------------------------------------------------------------------- Poly Pizza

PP_API = "https://api.poly.pizza/v1.1"
PP_CATEGORIES = ["Food & Drink", "Clutter", "Weapons", "Transport", "Furniture & Decor",
                 "Objects", "Nature", "Animals", "Buildings", "People & Characters",
                 "Scenes & Levels", "Other"]


def PolyPizzaKey():
    key = os.environ.get("POLY_PIZZA_API_KEY", "").strip()
    if not key:
        p = pathlib.Path("~/.config/monolith/poly-pizza.key").expanduser()
        if p.is_file():
            key = p.read_text().strip()
    if not key:
        print("""Poly Pizza needs the developer's own API key (its terms allow automated access only
through the published API; the website must not be scraped).
  1. Log in at https://poly.pizza and create an app at https://poly.pizza/settings/api
  2. Put the key into the environment as POLY_PIZZA_API_KEY, or into
     ~/.config/monolith/poly-pizza.key (chmod 600; never into the repo or a bundle).
Without a key: propose the model page URL (https://poly.pizza/m/<id>) in result.md and let the
developer download it by hand.""", file=sys.stderr)
        sys.exit(2)
    return key


def PPLicence(m):
    """(kind, licence, licence URL) from a model record; kind is 'cc0', 'ccby' or None."""
    lic = (m.get("Licence") or m.get("License") or "").strip()
    url = None
    mm = re.search(r"Licence at (https://\S+?)\.?$", m.get("Attribution") or "")
    if mm:
        url = mm.group(1)
    low = lic.lower().replace(" ", "")
    if low.startswith("cc0"):
        return "cc0", lic, url or CC0_URL
    if re.fullmatch(r"cc-?by[0-9.]*", low):
        ver = re.sub(r"[^0-9.]", "", low) or "3.0"
        return "ccby", lic, url or "https://creativecommons.org/licenses/by/%s/" % ver
    return None, lic, url              # CC-BY-SA / NC / ND / unknown: refused


def PPRow(m):
    kind, lic, _ = PPLicence(m)
    return "%-12s %-32.32s %7s  %-10s %-20.20s %s" % (
        m.get("ID", "?"), m.get("Title", ""), m.get("Tri Count", "?"), lic or "?",
        (m.get("Creator") or {}).get("Username", "?"),
        "" if kind else "(licence not accepted)")


def PPLoad(a, path):
    if a.offline_json:                 # parsing tests against a saved response
        with open(a.offline_json) as f:
            return json.load(f)
    return GetJson(PP_API + path, HOSTS["polypizza"], {"x-auth-token": PolyPizzaKey()})


def PolyPizzaSearch(a):
    params = {"Limit": min(a.limit, 32)}
    if a.license:
        params["License"] = 1 if a.license == "cc0" else 0
    if a.category is not None:
        params["Category"] = a.category
    path = "/search/" + urllib.parse.quote(" ".join(a.text), safe="") + "?" + urllib.parse.urlencode(params)
    data = PPLoad(a, path)
    res = data.get("results", [])
    if a.max_tris:
        res = [m for m in res if (m.get("Tri Count") or 0) <= a.max_tris]
    print("Poly Pizza: %s total, %d shown (API v1.1; licence per model)" % (data.get("total"), len(res)))
    print("%-12s %-32s %7s  %-10s %-20s" % ("id", "title", "tris", "licence", "creator"))
    for m in res:
        print(PPRow(m))


def PolyPizzaInfo(a):
    m = PPLoad(a, "/model/" + PPId(a.id))
    kind, lic, url = PPLicence(m)
    print(PPRow(m))
    print("page:        https://poly.pizza/m/%s" % m.get("ID"))
    print("licence:     %s (%s)%s" % (lic, url, "" if kind else "  NOT ACCEPTED: CC0 / CC-BY only"))
    print("attribution: %s" % m.get("Attribution"))
    print("category:    %s   animated: %s   uploaded: %s" % (m.get("Category"), m.get("Animated"),
                                                           m.get("Uploaded")))
    print("download:    %s" % m.get("Download"))


def PPId(ident):
    if not re.fullmatch(r"[A-Za-z0-9_-]{4,32}", ident):
        raise Fail("bad Poly Pizza model id: " + ident)
    return ident


def PolyPizzaGet(a):
    ident = PPId(a.id)
    expected = Expected(a)
    m = PPLoad(a, "/model/" + ident)       # fetched right before the download: the licence
    kind, lic, licurl = PPLicence(m)        # that applies is the one at download time
    if not kind:
        raise Fail("licence %r is not accepted (CC0 or CC-BY only); nothing downloaded" % lic)
    dl = m.get("Download") or ""
    CheckUrl(dl, {"static.poly.pizza"})
    if not urllib.parse.urlsplit(dl).path.lower().endswith(".glb"):
        raise Fail("download is not a .glb: " + dl)
    if a.offline_json:
        print("offline: licence %s accepted, would download %s" % (lic, dl))
        return
    out = OutDir(a.out, "polypizza", ident)
    name = re.sub(r"[^A-Za-z0-9_-]+", "_", m.get("Title") or ident).strip("_") or ident
    size, md5, sha, final = Download(dl, HOSTS["polypizza"], out / (name + ".glb"))
    bad = Mismatches(expected, {"md5": md5, "sha256": sha})
    if bad:
        Refuse(out, "polypizza %s" % ident, bad)
    if expected:
        print("expected hash matches: %s.glb" % name)
    creator = (m.get("Creator") or {}).get("Username")
    WriteProvenance(out, {
        "source": "Poly Pizza", "assetId": ident, "title": m.get("Title"),
        "pageUrl": "https://poly.pizza/m/" + ident, "apiUrls": [PP_API + "/model/" + ident],
        "authors": [creator] if creator else [],
        "licence": lic, "licenceUrl": licurl,
        "attribution": m.get("Attribution") if kind == "ccby" else None,
        "apiAttribution": m.get("Attribution"), "polycount": m.get("Tri Count"),
        "animated": m.get("Animated"), "category": m.get("Category"),
        "files": [{"path": name + ".glb", "url": dl, "finalUrl": final, "size": size,
                   "md5": md5, "sha256": sha}]})
    if kind == "ccby":
        print("CC-BY: put this attribution into ASSETS.md and the sidecar:\n  " + str(m.get("Attribution")))


PP_SEEDS = [
    # the category names' words, then common game-asset nouns; the sweep adds the models' own tag words
    "food", "drink", "clutter", "weapon", "transport", "furniture", "decor", "object", "nature", "animal",
    "building", "people", "character", "scene", "level", "other",
    "tree", "rock", "plant", "flower", "grass", "bush", "mushroom", "house", "wall", "door", "window", "roof",
    "fence", "bridge", "tower", "castle", "car", "truck", "boat", "ship", "plane", "train", "bike", "chair",
    "table", "bed", "lamp", "shelf", "box", "crate", "barrel", "bottle", "cup", "plate", "fruit", "sword",
    "gun", "shield", "bow", "axe", "tool", "dog", "cat", "bird", "fish", "horse", "robot", "man", "woman",
    "zombie", "monster", "space", "planet", "rocket", "computer", "phone", "book", "sign", "street", "road",
    "light", "kitchen", "bathroom", "office", "school", "shop", "city", "farm", "forest", "desert", "snow",
    "water", "fire", "medieval", "fantasy", "pirate", "military", "sport", "ball", "toy", "music", "instrument",
]
PP_PAGE = 32                            # /search answers at most 32 results per page
PP_LICENCE_PARAM = (("cc0", 1), ("ccby", 0))


def PPCatalogLicence(m):
    """(licence, licence URL) for a catalog row, or None for a licence the catalog doesn't index."""
    lic = (m.get("Licence") or m.get("License") or "").strip()
    low = lic.lower().replace(" ", "").replace("-", "")
    if low in ("cc0", "cc01.0"):
        return "CC0 1.0", CC0_URL
    mm = re.fullmatch(r"ccby([34])\.0", low)
    if mm:
        return "CC BY %s.0" % mm.group(1), "https://creativecommons.org/licenses/by/%s.0/" % mm.group(1)
    return None


class PPBudget(Fail):
    """The sweep reached --max-requests: a normal end, the models found so far are the catalog."""


class PPSweep:
    """Enumerates Poly Pizza's models through the official API v1.1. /search needs a keyword, pages with
    Page (0-based) and Limit (at most 32) and reports `total`; many keywords answer HTTP 500 for the whole
    query (any page), so a keyword is searched per licence, and per licence and category where that fails.
    /user/<name> gives a creator's 32 newest models and their lists; /list/<id> a list's models."""

    def __init__(self, key, estimate=False, budget=0):
        self.key, self.estimate = key, estimate
        # the free plan allows 3000 API requests a month per key (failed ones count too), shared with
        # `polypizza get` in request sessions, so a sweep stops at its budget instead of using the key up
        self.budget, self.used = budget, 0
        self.models, self.order = {}, []
        self.searched, self.failed, self.pages = set(), [], 0
        self.expected_pages = 0

    def get(self, path):
        if self.budget and self.used >= self.budget:
            raise PPBudget("request budget of %d reached" % self.budget)
        self.used += 1
        try:
            return GetJson(PP_API + path, HOSTS["polypizza"], {"x-auth-token": self.key})
        except HttpFail as e:
            if e.code in (401, 403):
                raise Fail("Poly Pizza refused the API key (HTTP %d): the key is wrong, or its monthly request "
                           "quota (free plan: 3000) is used up (see the app's page on poly.pizza); check "
                           "POLY_PIZZA_API_KEY or ~/.config/monolith/poly-pizza.key; stopping" % e.code)
            if e.code in (500, 502, 503):
                return None
            raise

    def add(self, ms):
        new = 0
        for m in ms or []:
            if not isinstance(m, dict):
                continue
            try:
                ident = PPId(str(m.get("ID")))
            except Fail:
                continue
            if ident not in self.models:
                self.models[ident] = m
                self.order.append(ident)
                new += 1
        return new

    def search(self, word, params):
        """Every page of one query; False when the API answers 500."""
        q = "/search/" + urllib.parse.quote(word, safe="") + "?"
        first = self.get(q + urllib.parse.urlencode({**params, "Limit": PP_PAGE, "Page": 0}))
        if first is None:
            return False
        total = first.get("total") or 0
        pages = -(-total // PP_PAGE)
        self.expected_pages += max(pages, 1)
        self.add(first.get("results"))
        if self.estimate:
            return True
        for page in range(1, pages):
            d = self.get(q + urllib.parse.urlencode({**params, "Limit": PP_PAGE, "Page": page}))
            if d is None:
                self.failed.append("%s %s page %d" % (word, params, page))
                continue
            if not d.get("results"):
                break
            self.add(d.get("results"))
        return True

    def word(self, word):
        self.searched.add(word)
        for name, lic in PP_LICENCE_PARAM:
            if self.search(word, {"License": lic}):
                continue
            for cat in range(len(PP_CATEGORIES)):
                if not self.search(word, {"License": lic, "Category": cat}):
                    self.failed.append("%s %s %s" % (word, name, PP_CATEGORIES[cat]))

    def next_words(self, min_count=2):
        """Unsearched tag and title words of the models found, most frequent first."""
        count = {}
        for m in self.models.values():
            words = set(re.findall(r"[a-z]{3,}", " ".join([m.get("Title") or ""] + [
                t for t in m.get("Tags") or [] if isinstance(t, str)]).lower()))
            for w in words - self.searched:
                count[w] = count.get(w, 0) + 1
        return [w for w, c in sorted(count.items(), key=lambda kv: (-kv[1], kv[0])) if c >= min_count]

    def creators(self):
        names = sorted({(m.get("Creator") or {}).get("Username") for m in self.models.values()} - {None, ""})
        lists = set()
        for name in names:
            d = self.get("/user/" + urllib.parse.quote(name, safe=""))
            if d is None:
                self.failed.append("user " + name)
                continue
            self.add(d.get("models"))
            lists.update(x for x in d.get("lists") or [] if isinstance(x, str) and re.fullmatch(r"[A-Za-z0-9_-]{4,32}", x))
        for lid in sorted(lists):
            d = self.get("/list/" + lid)
            if d is None:
                self.failed.append("list " + lid)
                continue
            self.add(d.get("Models"))
        return len(names), len(lists)


def PolyPizzaCatalog(a):
    """Sweeps the API (seed keywords, then the models' own tag words, then every creator's newest models and
    lists), deduplicated by id; writes <out>/polypizza.jsonl with the CC0 and CC BY rows. The key is sent only
    in the x-auth-token header and never printed or written."""
    t0 = time.monotonic()
    out = CatalogDir(a.out)
    sw = PPSweep(PolyPizzaKey(), estimate=a.estimate, budget=a.max_requests)
    phase_new = []
    stopped = None
    try:
        words = list(PP_SEEDS)
        for w in words:
            before = len(sw.models)
            sw.word(w)
            if a.limit and len(sw.models) >= a.limit:
                break
            if a.verbose:
                print("  %-16s +%d = %d" % (w, len(sw.models) - before, len(sw.models)), file=sys.stderr)
        phase_new.append(("seed keywords", len(words), len(sw.models)))
        if a.estimate:
            print("estimate: %d seed keywords need %d search pages (%d answered 500 even split by category)"
                  % (len(sw.searched), sw.expected_pages, len(sw.failed)))
            print("estimate: %d unique models on the first pages" % len(sw.models))
            RequestSummary(t0)
            return
        if not a.limit:
            rounds = 0
            while rounds < a.max_words:
                nxt = sw.next_words()[:a.max_words - rounds]
                if not nxt:
                    break
                for w in nxt:
                    sw.word(w)
                    rounds += 1
                print("  tag words: %d searched, %d models" % (rounds, len(sw.models)), file=sys.stderr)
            phase_new.append(("tag words", rounds, len(sw.models)))
            nc, nl = sw.creators()
            phase_new.append(("creators %d, lists %d" % (nc, nl), nc + nl, len(sw.models)))
    except PPBudget as e:
        phase_new.append(("stopped at the budget: %s" % e, 0, len(sw.models)))
    except Fail as e:
        # the sweep stopped (a refused key, the API giving up): keep the models found so far in a
        # separate .partial.jsonl, so hours of requests aren't lost, then fail as before
        stopped = e
        phase_new.append(("stopped early: %s" % e, 0, len(sw.models)))
    ids = sorted(sw.models)
    if a.limit:
        ids = ids[:a.limit]
    rows, skipped = [], {}
    for ident in ids:
        m = sw.models[ident]
        lic = PPCatalogLicence(m)
        att = (m.get("Attribution") or "").strip()
        dl = m.get("Download") or ""
        why = None
        if not lic:
            why = "licence %r" % (m.get("Licence") or m.get("License"))
        elif lic[0].startswith("CC BY") and not att:
            why = "CC BY without attribution"
        else:
            try:
                CheckUrl(dl, {"static.poly.pizza"})
                if not urllib.parse.urlsplit(dl).path.lower().endswith(".glb"):
                    why = "download is not a .glb"
            except Fail:
                why = "download outside static.poly.pizza"
        if why:
            skipped[why] = skipped.get(why, 0) + 1
            continue
        tags = [t for t in m.get("Tags") or [] if isinstance(t, str)]
        cat = m.get("Category") if isinstance(m.get("Category"), str) else None
        thumb = m.get("Thumbnail") if isinstance(m.get("Thumbnail"), str) else None
        rows.append({
            "id": "polypizza-" + ident, "source": "polypizza", "name": m.get("Title") or ident,
            "collection": None, "page_url": "https://poly.pizza/m/" + ident, "file_url": dl,
            "path_in_zip": None, "file_hash": None, "hash_kind": None, "zip_sha256": None,
            "licence": lic[0], "licence_url": lic[1], "attribution": att if lic[0].startswith("CC BY") else None,
            "authors": (m.get("Creator") or {}).get("Username") or None, "rigged": int(m.get("Animated") is True),
            "ik": 0, "tags": TagWords(cat, *tags, m.get("Title")), "tris": m.get("Tri Count"),
            "sx": None, "sy": None, "sz": None, "slots": None, "flat_shaded": None, "textured": None,
            "factor_colours_only": None, "preview": thumb, "source_tags": ([cat] if cat else []) + tags})
    for phase, n, total in phase_new:
        print("%s (%d): %d unique models so far" % (phase, n, total))
    print("API requests used: %d of a budget of %s" % (sw.used, sw.budget or "no limit"))
    print("search pages: %d; queries that answered 500 even split by category: %d"
          % (sw.expected_pages, len(sw.failed)))
    for why, n in sorted(skipped.items()):
        print("skipped %d: %s" % (n, why))
    lic = {}
    for r in rows:
        lic[r["licence"]] = lic.get(r["licence"], 0) + 1
    print("licences: " + ", ".join("%s %d" % kv for kv in sorted(lic.items())))
    if stopped is not None:
        WriteJsonl(out / "polypizza.partial.jsonl", rows)
        print("wrote the %d rows found before it stopped to %s" % (len(rows), out / "polypizza.partial.jsonl"))
        RequestSummary(t0)
        raise stopped
    WriteJsonl(out / "polypizza.jsonl", rows)
    RequestSummary(t0)


# ----------------------------------------------------------------------------- Polyfork

PF_API = "https://polyfork.dev/api"
PF_TERMS = "https://polyfork.dev/licensing"


def PFId(ident):
    if not re.fullmatch(r"[a-z0-9]+(-[a-z0-9]+)*", ident or "") or len(ident) > 120:
        raise Fail("bad Polyfork asset id: %r" % ident)
    return ident


def PFGlbUrl(ident):
    return "https://polyfork.dev/cdn/%s.glb" % ident


def PFCredit(detail, ident):
    """The CC BY 4.0 credit line, after checking that the API still offers the mesh files under CC BY 4.0."""
    lic = detail.get("license") or ""
    credit = (detail.get("credit") or "").strip()
    if "CC BY 4.0" not in lic or "CC BY 4.0" not in credit:
        raise Fail("%s: the API no longer offers the mesh under CC BY 4.0 (license %r, credit %r)"
                   % (ident, lic[:120], credit[:120]))
    if detail.get("free") is not True or (detail.get("download") or {}).get("glb") != PFGlbUrl(ident):
        raise Fail("%s: not a free model with %s" % (ident, PFGlbUrl(ident)))
    return credit


def PolyforkCatalog(a):
    """Every free model: GET /api/assets?free=1 (pages of 100, newest first, no cursor) for the facts, then
    /api/assets/<id> for materials, credit and the views.png render (the preview, never stored). Polyfork's
    .mjs, HD editions, renders, description, palette, style text and params are never downloaded or kept.
    Writes <out>/polyfork.jsonl."""
    t0 = time.monotonic()
    out = CatalogDir(a.out)
    hosts = HOSTS["polyfork"]
    listed, page, total = {}, 1, None
    while True:
        d = GetJson(PF_API + "/assets?" + urllib.parse.urlencode(
            {"free": 1, "per_page": 100, "page": page, "sort": "latest"}), hosts)
        total = d.get("total")
        per = d.get("per_page") or 100
        for x in d.get("assets") or []:
            listed[PFId(x.get("id"))] = x       # the order shifts while new models are published: keep by id
        if not d.get("assets") or not isinstance(total, int) or page * per >= total:
            break
        page += 1
    print("Polyfork: %d free models listed (total %s, %d pages)" % (len(listed), total, page))
    if isinstance(total, int) and len(listed) < total:
        print("warning: %d listed, the API says %d (models removed while paging?)" % (len(listed), total))
    ids = sorted(listed)
    if a.limit:
        ids = ids[:a.limit]
    rows, skipped = [], []
    for n, ident in enumerate(ids, 1):
        x = listed[ident]
        det = GetJson(PF_API + "/assets/" + ident, hosts)
        if det.get("id") != ident:
            skipped.append((ident, "detail answer has another id"))
            continue
        try:
            credit = PFCredit(det, ident)
        except Fail as e:
            skipped.append((ident, str(e)))
            continue
        page_url = "https://polyfork.dev/asset/" + ident
        if x.get("page") != page_url:
            skipped.append((ident, "unexpected page URL %r" % x.get("page")))
            continue
        views = [r for r in det.get("renders") or []
                 if isinstance(r, str) and r == "https://polyfork.dev/files/%s/renders/views.png" % ident]
        size = x.get("size_m") or {}
        kit, cls, title = x.get("kit"), x.get("class"), x.get("title") or ident
        rows.append({
            "id": "polyfork-" + ident, "source": "polyfork", "name": title, "collection": kit,
            "page_url": page_url, "file_url": PFGlbUrl(ident), "path_in_zip": None,
            "file_hash": None, "hash_kind": None, "zip_sha256": None,
            "licence": "CC BY 4.0", "licence_url": CC_BY_4_URL, "attribution": credit, "authors": "Polyfork",
            "rigged": int(bool(x.get("has_rig"))), "ik": int(bool(x.get("has_ik"))),
            "tags": " ".join(sorted(set(TagWords(cls, title).split()) | ({kit.lower()} if kit else set()))),
            "tris": x.get("triangles"), "sx": Size(size.get("x")), "sy": Size(size.get("y")),
            "sz": Size(size.get("z")), "slots": det.get("materials"), "flat_shaded": 1, "textured": None,
            "factor_colours_only": None, "preview": views[0] if views else None,
            "source_tags": [t for t in (cls, kit) if t]})
        if n % 50 == 0:
            print("  %d / %d" % (n, len(ids)), file=sys.stderr)
    for ident, why in skipped:
        print("skipped %s: %s" % (ident, why))
    nulls = {k: sum(1 for r in rows if r[k] is None) for k in ("collection", "tris", "sx", "slots", "preview")}
    print("NULL counts: " + ", ".join("%s %d" % kv for kv in nulls.items()))
    WriteJsonl(out / "polyfork.jsonl", rows)
    RequestSummary(t0)


def PolyforkGet(a):
    """The free GLB only (https://polyfork.dev/cdn/<id>.glb, no key), with the credit from /api/assets/<id>."""
    ident = PFId(a.id)
    expected = Expected(a)
    hosts = HOSTS["polyfork"]
    det = GetJson(PF_API + "/assets/" + ident, hosts)     # the licence at download time is the one that applies
    credit = PFCredit(det, ident)
    out = OutDir(a.out, "polyfork", ident)
    url = PFGlbUrl(ident)
    size, md5, sha, final = Download(url, hosts, out / (ident + ".glb"), final_suffix=".glb")
    rec = {"path": ident + ".glb", "url": url, "finalUrl": final, "size": size, "md5": md5, "sha256": sha}
    bad = Mismatches(expected, rec)
    if bad:
        Refuse(out, "polyfork %s" % ident, bad)
    if expected:
        print("expected hash matches: %s.glb" % ident)
    WriteProvenance(out, {
        "source": "Polyfork", "assetId": ident, "title": det.get("title"),
        "pageUrl": "https://polyfork.dev/asset/" + ident, "apiUrls": [PF_API + "/assets/" + ident],
        "authors": ["Polyfork"], "licence": "CC-BY 4.0", "licenceUrl": CC_BY_4_URL,   # the spelling record_asset.py accepts
        "licenceTerms": PF_TERMS, "attribution": credit, "polycount": det.get("triangles"),
        "files": [rec]})
    print("CC BY 4.0: put this attribution into ASSETS.md and the sidecar:\n  " + credit)
    if not expected:
        print("no --expect-sha256: record the hash with `library pin polyfork-%s %s`" % (ident, sha))


# ----------------------------------------------------------------------------- Hugging Face

HF_EMBED_DIR = "~/.cache/monolith/embed"


def HuggingFaceGet(a):
    """One file of a model repo (only for the model library's embedding model), refused unless its sha256 is
    the expected one. Default folder: $MONOLITH_EMBED_DIR or ~/.cache/monolith/embed, which keeps other
    files, so this writes <file>, <file>.provenance.json and, on a mismatch, only <file>.REFUSED.txt."""
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", a.repo) or ".." in a.repo:
        raise Fail("bad repo name: " + a.repo)
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", a.revision) or ".." in a.revision:
        raise Fail("bad revision: " + a.revision)
    rel = SafeRel(a.file)
    if rel is None or len(rel.parts) != 1:
        raise Fail("bad file name (one file at the top of the repo): " + a.file)
    expected = Expected(a)
    out = pathlib.Path(a.out or os.environ.get("MONOLITH_EMBED_DIR") or HF_EMBED_DIR).expanduser().resolve()
    if out == REPO or REPO in out.parents:
        raise Fail("the embedding model stays outside the repo, not in %s" % out)
    out.mkdir(parents=True, exist_ok=True)
    dest = out / rel.name
    for f in (dest, out / (rel.name + ".part"), out / (rel.name + ".provenance.json")):
        if f.exists():
            raise Fail("%s exists: move it to the trash first if it should be replaced" % f)
    url = "https://huggingface.co/%s/resolve/%s/%s" % (a.repo, a.revision, urllib.parse.quote(rel.name))
    part = out / (rel.name + ".part")
    size, md5, sha, final = Download(url, HOSTS["huggingface"], part, limit=MAX_HF_FILE)
    rec = {"path": rel.name, "url": url, "finalUrl": final.split("?")[0], "size": size, "md5": md5,
           "sha256": sha}
    bad = Mismatches(expected, rec)
    if bad:
        Refuse(out, "huggingface %s %s" % (a.repo, rel.name), bad, files=[part], note=rel.name + ".REFUSED.txt")
    os.replace(part, dest)
    print("expected hash matches: %s (%d bytes)" % (dest, size))
    prov = {"tool": "tools/fetch_asset.py", "toolVersion": TOOL_VERSION, "downloadedAt": Now(),
            "source": "Hugging Face", "repo": a.repo, "revision": a.revision,
            "pageUrl": "https://huggingface.co/" + a.repo, "files": [rec]}
    with open(out / (rel.name + ".provenance.json"), "x") as f:
        json.dump(prov, f, indent=2)
        f.write("\n")
    print("provenance: " + str(out / (rel.name + ".provenance.json")))


# ----------------------------------------------------------------------------- main

def main():
    p = argparse.ArgumentParser(description="Licence-safe asset downloads for monolith map packages")
    src = p.add_subparsers(dest="source", required=True)

    ph = src.add_parser("polyhaven").add_subparsers(dest="cmd", required=True)
    s = ph.add_parser("search")
    s.add_argument("text", nargs="*")
    s.add_argument("--category")
    s.add_argument("--max-tris", type=int)
    s.add_argument("--limit", type=int, default=40)
    s.set_defaults(fn=PolyHavenSearch)
    g = ph.add_parser("get")
    g.add_argument("slug")
    g.add_argument("--res", default="1k", choices=["1k", "2k", "4k", "8k"])
    g.add_argument("--out")
    AddExpectArgs(g, what="the .gltf")
    g.set_defaults(fn=PolyHavenGet)
    c = ph.add_parser("catalog")
    c.add_argument("--out", required=True, help="scratch folder for polyhaven.jsonl (outside the repo)")
    c.add_argument("--limit", type=int, help="only the first N models by slug (pilot runs)")
    c.set_defaults(fn=PolyHavenCatalog)

    ac = src.add_parser("ambientcg").add_subparsers(dest="cmd", required=True)
    s = ac.add_parser("search")
    s.add_argument("text", nargs="+")
    s.add_argument("--limit", type=int, default=20)
    s.set_defaults(fn=AmbientSearch)
    g = ac.add_parser("get")
    g.add_argument("id")
    g.add_argument("--res", default="1K", choices=["1K", "2K", "4K", "8K", "1k", "2k", "4k", "8k"])
    g.add_argument("--out")
    g.set_defaults(fn=AmbientGet)

    kn = src.add_parser("kenney", help="reads the local zip cache only ($MONOLITH_KENNEY_ZIPS or "
                        "~/.cache/monolith/kenney); never fetches from kenney.nl").add_subparsers(dest="cmd", required=True)
    s = kn.add_parser("list")
    s.add_argument("pack", nargs="?", help="list this pack's models (default: the cached packs)")
    s.set_defaults(fn=KenneyList)
    g = kn.add_parser("get")
    g.add_argument("target", help="the catalog's path_in_zip, or its id kenney-<pack>/<name>")
    g.add_argument("--pack", help="the pack slug (needed with a path when the zip has changed)")
    g.add_argument("--expect-zip-sha256", metavar="HEX", required=True,
                   help="the cached zip's sha256; another zip of the pack is used only if the entry still matches")
    g.add_argument("--expect-sha256", metavar="HEX", required=True, help="refuse unless the entry's sha256 is this")
    g.add_argument("--out")
    g.set_defaults(fn=KenneyGet)
    c = kn.add_parser("catalog")
    c.add_argument("zip", nargs="*", help="pack or bundle zips (default: every *.zip in the cache folder)")
    c.add_argument("--out", required=True, help="scratch folder for kenney.jsonl (outside the repo)")
    c.add_argument("--monolith", default=str(REPO / "build" / "linux" / "monolith"),
                   help="the monolith binary used for --inspect-model (default: this checkout's build)")
    c.set_defaults(fn=KenneyCatalog)

    pp = src.add_parser("polypizza").add_subparsers(dest="cmd", required=True)
    s = pp.add_parser("search")
    s.add_argument("text", nargs="+")
    s.add_argument("--license", choices=["cc0", "ccby"])
    s.add_argument("--category", type=int, choices=range(len(PP_CATEGORIES)),
                   help="; ".join("%d %s" % x for x in enumerate(PP_CATEGORIES)))
    s.add_argument("--max-tris", type=int)
    s.add_argument("--limit", type=int, default=32)
    s.set_defaults(fn=PolyPizzaSearch)
    for name, fn in (("info", PolyPizzaInfo), ("get", PolyPizzaGet)):
        g = pp.add_parser(name)
        g.add_argument("id")
        if name == "get":
            g.add_argument("--out")
            AddExpectArgs(g, what="the .glb")
        g.set_defaults(fn=fn)
    for sp in (pp.choices["search"], pp.choices["info"], pp.choices["get"]):
        sp.add_argument("--offline-json", help=argparse.SUPPRESS)   # parse a saved response (tests)
    c = pp.add_parser("catalog")
    c.add_argument("--out", required=True, help="scratch folder for polypizza.jsonl (outside the repo)")
    c.add_argument("--limit", type=int, help="stop the sweep at N models and keep the first N by id (pilot runs)")
    c.add_argument("--max-words", type=int, default=3000, help="tag words searched after the seed keywords")
    c.add_argument("--max-requests", type=int, default=2000,
                   help="stop the sweep after this many API requests (free plan: 3000 a month per key, shared "
                        "with polypizza get; 0 = no limit)")
    c.add_argument("--estimate", action="store_true",
                   help="only the first page of each seed keyword: prints the pages a full sweep of them needs")
    c.add_argument("--verbose", action="store_true")
    c.set_defaults(fn=PolyPizzaCatalog)

    pf = src.add_parser("polyfork").add_subparsers(dest="cmd", required=True)
    c = pf.add_parser("catalog")
    c.add_argument("--out", required=True, help="scratch folder for polyfork.jsonl (outside the repo)")
    c.add_argument("--limit", type=int, help="only the first N models by id (pilot runs)")
    c.set_defaults(fn=PolyforkCatalog)
    g = pf.add_parser("get")
    g.add_argument("id")
    g.add_argument("--out")
    AddExpectArgs(g, what="the .glb")
    g.set_defaults(fn=PolyforkGet)

    hf = src.add_parser("huggingface").add_subparsers(dest="cmd", required=True)
    g = hf.add_parser("get", help="the model library's embedding model only")
    g.add_argument("repo", help="e.g. Qwen/Qwen3-Embedding-0.6B-GGUF")
    g.add_argument("file", help="e.g. Qwen3-Embedding-0.6B-Q8_0.gguf")
    g.add_argument("--expect-sha256", metavar="HEX", required=True, help="refuse unless the file's sha256 is this")
    g.add_argument("--revision", default="main", help="branch, tag or commit (default main)")
    g.add_argument("--out", help="folder (default $MONOLITH_EMBED_DIR or ~/.cache/monolith/embed)")
    g.set_defaults(fn=HuggingFaceGet)

    a = p.parse_args()
    try:
        a.fn(a)
    except Fail as e:
        print("fetch_asset: " + str(e), file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
