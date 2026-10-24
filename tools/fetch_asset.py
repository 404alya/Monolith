#!/usr/bin/env python3
# fetch_asset.py -- licence-safe downloads of models and textures for monolith map packages
#
# Run with `python3 -I tools/fetch_asset.py <source> <command> ...` (stdlib only; -I keeps the
# current directory and user site-packages off sys.path). See .claude/skills/assets-and-licences/SKILL.md.
#
#   polyhaven search [text] [--category c] [--max-tris n] [--limit n]   CC0 models (API)
#   polyhaven get <slug> [--res 1k|2k|4k] [--out DIR]   glTF + .bin + textures, md5-verified
#   ambientcg search <text> [--limit n]                 CC0 materials (API)
#   ambientcg get <id> [--res 1K|2K] [--out DIR]        texture zip; extracts image files only
#   kenney list <zip url> [--out DIR]                   download a pack zip, audit, list entries
#   kenney get <zip url> --pick GLOB [--pick ...] [--out DIR]   extract the picked files + License.txt
#   polypizza search <keyword> [--license cc0|ccby] [--category n] [--max-tris n] [--limit n]
#   polypizza info <id>
#   polypizza get <id> [--out DIR]                      GLB; only CC0 / CC-BY, recorded at download
#
# Safety rules built in (they are the project's hard rules for downloads):
# - every download goes into a NEW, EMPTY directory outside the repo (default:
#   $MONOLITH_DOWNLOADS or ~/.cache/monolith/downloads, then <source>-<id>-<stamp>/);
# - https only, redirects only to the source's own hosts, a size cap per file;
# - archive entries are audited (absolute paths, "..", backslashes, symlinks, sizes) and only
#   model / texture / licence files are written; nothing from a download is ever executed;
# - every download writes provenance.json next to the files: source, URLs, author(s),
#   licence + URL, date, md5 (when the API gives one) and sha256 of every file;
# - Poly Pizza only through its official API v1.1 (https://api.poly.pizza/v1.1, header
#   x-auth-token) with the developer's key from $POLY_PIZZA_API_KEY or
#   ~/.config/monolith/poly-pizza.key. The key is never printed or written anywhere.

import argparse
import datetime
import fnmatch
import hashlib
import json
import os
import pathlib
import re
import stat
import sys
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
REPO = pathlib.Path(__file__).resolve().parent.parent

HOSTS = {
    "polyhaven": {"api.polyhaven.com", "dl.polyhaven.org"},
    "ambientcg": {"ambientcg.com", "acg-download.struffelproductions.com"},
    "kenney": {"kenney.nl", "www.kenney.nl"},
    "polypizza": {"api.poly.pizza", "static.poly.pizza"},
}
MODEL_EXT = {".glb", ".gltf", ".bin", ".obj", ".mtl", ".fbx", ".dae"}
IMAGE_EXT = {".png", ".jpg", ".jpeg"}
LICENCE_NAMES = {"license.txt", "licence.txt", "license.md", "license", "copying.txt"}

CC0_URL = "https://creativecommons.org/publicdomain/zero/1.0/"


class Fail(Exception):
    pass


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


def Open(url, hosts, headers=None):
    CheckUrl(url, hosts)
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, **(headers or {})})
    opener = urllib.request.build_opener(_Redirects(hosts))
    try:
        return opener.open(req, timeout=60)
    except urllib.error.HTTPError as e:
        raise Fail("HTTP %d for %s" % (e.code, Redact(url)))
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


def Download(url, hosts, dest, limit=MAX_FILE):
    """Stream url into dest (must not exist). Returns (size, md5, sha256, final url)."""
    if dest.exists():
        raise Fail("refusing to overwrite " + str(dest))
    md5, sha = hashlib.md5(), hashlib.sha256()
    size = 0
    with Open(url, hosts) as r:
        final = r.geturl()
        CheckUrl(final, hosts)
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


def Extract(z, info, rel, dest_root):
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
            if size > MAX_ENTRY:
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
        if f.get("md5") and md5 != f["md5"]:
            raise Fail("md5 mismatch for %s: API %s, got %s" % (rel, f["md5"], md5))
        recs.append({"path": str(rel), "url": f["url"], "size": size,
                     "md5": md5, "md5Verified": bool(f.get("md5")), "sha256": sha})
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

def KenneyZip(a):
    url = a.url
    CheckUrl(url, HOSTS["kenney"])
    if not urllib.parse.urlsplit(url).path.lower().endswith(".zip"):
        raise Fail("expected a .zip URL from kenney.nl (the pack page's download link)")
    name = SafeRel(pathlib.PurePosixPath(urllib.parse.urlsplit(url).path).name)
    if name is None:
        raise Fail("odd zip name in URL")
    out = OutDir(a.out, "kenney", name.stem)
    zdir = out / "archive"
    size, md5, sha, final = Download(url, HOSTS["kenney"], zdir / name.name)
    z, entries = AuditZip(zdir / name.name)
    return out, z, entries, {"path": "archive/" + name.name, "url": url, "finalUrl": final,
                             "size": size, "md5": md5, "sha256": sha}


def KenneyList(a):
    out, z, entries, rec = KenneyZip(a)
    for info, rel in entries:
        if rel.suffix.lower() in MODEL_EXT | IMAGE_EXT or rel.name.lower() in LICENCE_NAMES:
            print("  %8d  %s" % (info.file_size, rel))
    print("zip kept in %s; run `kenney get <url> --pick '<glob>' --out <another new dir>`" % out)


def KenneyGet(a):
    out, z, entries, rec = KenneyZip(a)
    recs, licence_text = [rec], None
    picked = 0
    for info, rel in entries:
        is_lic = rel.name.lower() in LICENCE_NAMES
        ok = rel.suffix.lower() in MODEL_EXT | IMAGE_EXT
        if not is_lic and not (ok and any(fnmatch.fnmatch(str(rel), p) for p in a.pick)):
            continue
        if is_lic and len(rel.parts) > 2:
            continue                    # the pack's top-level licence only
        dest = Extract(z, info, rel, out / "extracted")
        picked += 0 if is_lic else 1
        recs.append({"path": "extracted/" + str(rel), "fromArchive": rec["path"],
                     "size": dest.stat().st_size, "sha256": Sha256(dest)})
        if is_lic:
            licence_text = dest.read_text("utf-8", "replace")
    if not picked:
        raise Fail("--pick matched no model/texture file (see `kenney list`)")
    if not licence_text:
        raise Fail("no License.txt in the pack: don't use it until the licence is clear")
    cc0 = "creativecommons.org/publicdomain/zero" in licence_text or "CC0" in licence_text
    if not cc0:
        raise Fail("License.txt does not say CC0: read it and decide by hand")
    WriteProvenance(out, {
        "source": "Kenney", "assetId": pathlib.PurePosixPath(rec["path"]).stem,
        "title": pathlib.PurePosixPath(rec["path"]).stem, "pageUrl": a.page,
        "apiUrls": [], "authors": ["Kenney (www.kenney.nl)"],
        "licence": "CC0 1.0", "licenceUrl": CC0_URL,
        "licenceFile": next((r["path"] for r in recs if pathlib.PurePosixPath(r["path"]).name.lower()
                             in LICENCE_NAMES), None),
        "attribution": None, "files": recs})


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
    g.set_defaults(fn=PolyHavenGet)

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

    kn = src.add_parser("kenney").add_subparsers(dest="cmd", required=True)
    s = kn.add_parser("list")
    s.add_argument("url")
    s.add_argument("--out")
    s.set_defaults(fn=KenneyList)
    g = kn.add_parser("get")
    g.add_argument("url")
    g.add_argument("--pick", action="append", required=True,
                   help="glob on the entry path, e.g. 'Models/GLTF format/tree_pine*.glb'")
    g.add_argument("--page", help="the pack's page URL, for the record (e.g. https://kenney.nl/assets/nature-kit)")
    g.add_argument("--out")
    g.set_defaults(fn=KenneyGet)

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
        g.set_defaults(fn=fn)
    for sp in (pp.choices["search"], pp.choices["info"], pp.choices["get"]):
        sp.add_argument("--offline-json", help=argparse.SUPPRESS)   # parse a saved response (tests)

    a = p.parse_args()
    try:
        a.fn(a)
    except Fail as e:
        print("fetch_asset: " + str(e), file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
