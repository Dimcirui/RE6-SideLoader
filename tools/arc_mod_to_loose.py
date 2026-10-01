r"""Turn a "repacked arc" mod (.zip) into a side-loader mod (.zip) that holds only the files the mod really changed.

usage: python tools/arc_mod_to_loose.py <mod.zip> [more.zip ...] [-o out.zip] [--name Name] [--folder] [--db db.json.gz]

No game install is needed: the retail arcs are known from tools/data/re6_retail_db.json.gz (entry paths + MD5 of
the decompressed data, built with build_retail_db.py). Output layout, ready for a mod manager:

    nativePC_mod\data\...                    changed resource, same new version in every mod arc that has it
    nativePC_mod\arc\DX9\<arc>\data\...      only for that arc (several versions, retail kept in some mod arcs,
                                             or an entry the mod ADDED to the arc)
    nativePC_mod\arc\DX9\<new>.arc           arcs the game does not have (side-loaded whole)
    nativePC_mod\<path>                      loose files the mod ships (found below nativePC\ or data\ ...)
    nativePC_mod\_reports\<Name>.txt         why every file is where it is (folders starting with _ are not loaded)

No mod folder by default: mods that replace the same file conflict anyway, so the one installed last wins (the mod
manager overwrites the files). --folder puts everything into nativePC_mod\<Name>\ instead, as a separate mod.
"""
import argparse
import collections
import gzip
import hashlib
import io
import json
import sys
import zipfile
import zlib
from pathlib import Path, PurePosixPath

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))  # re6arc is bundled next to this file
from re6arc import arc  # noqa: E402

NATIVE_DIRS = {"data", "effect", "effect_dlc", "soft", "sound", "sound_dlc", "system", "sc", "etc", "movie"}
DOC_EXTS = {".txt", ".md", ".png", ".jpg", ".jpeg", ".gif", ".webp", ".pdf", ".url"}


def digest(data: bytes) -> str:
    return hashlib.md5(data).hexdigest()[:16]


class RetailDB:
    def __init__(self, path: Path):
        with gzip.open(path, "rt", encoding="utf-8") as fh:
            db = json.load(fh)
        self.info = {k: db[k] for k in ("game", "built", "exe_md5")}
        paths = db["paths"]
        self.arcs = {}                                   # "nativePC/arc/DX9/x.arc" -> {lower path: md5}
        self.by_name = collections.defaultdict(list)     # "x.arc" -> [arc rel paths]
        self.holders = collections.defaultdict(list)     # lower path -> [arc rel paths]
        for rel, rows in db["arcs"].items():
            self.arcs[rel] = {paths[i].lower(): h for i, h in rows}
            self.by_name[PurePosixPath(rel).name.lower()].append(rel)
            for i, _ in rows:
                self.holders[paths[i].lower()].append(rel)

    def find_arc(self, zip_path: str):
        """retail arc matching a mod arc: same file name; nativePC_dlc / sa\\ hints in the zip path pick among several"""
        cands = self.by_name.get(PurePosixPath(zip_path).name.lower(), [])
        if len(cands) <= 1:
            return cands[0] if cands else None
        low = zip_path.lower().replace("\\", "/")

        def score(rel):
            dlc = rel.startswith("nativePC_dlc/")
            s = 2 if (("nativepc_dlc/" in low) == dlc) else 0
            s += 1 if (("/sa/" in "/" + low) == ("/sa/" in rel.lower())) else 0
            return s
        return max(cands, key=score)


def arc_entries(data: bytes):
    """{lower path: (path, decompressed bytes)} of an arc held in memory; re6arc names files with the game's own
    extensions, so these are the names the game asks for when it loads a loose file"""
    import tempfile
    with tempfile.NamedTemporaryFile(suffix=".arc", delete=False) as t:
        t.write(data)
        tmp = Path(t.name)
    try:
        a = arc.parse(tmp)
        f = io.BytesIO(data)
        return {e.path.lower(): (e.path, arc.inflate(arc.read_raw(f, e), e)) for e in a.entries}
    finally:
        tmp.unlink()


def loose_target(zip_path: str):
    """path inside nativePC for a loose file of the mod zip, or None"""
    parts = PurePosixPath(zip_path.replace("\\", "/")).parts
    low = [p.lower() for p in parts]
    for i, p in enumerate(low):
        if p in ("nativepc", "nativepc_dlc", "nativepc_mod") and i + 1 < len(parts):
            return PurePosixPath(*parts[i + 1:])
    for i, p in enumerate(low[:-1]):
        if p in NATIVE_DIRS:
            return PurePosixPath(*parts[i:])
    return None


def convert(src: Path, out: Path, name: str, db: RetailDB, folder: bool = False):
    z = zipfile.ZipFile(src)
    files = {}        # path inside the mod folder -> bytes
    report = [f"source: {src.name}", f"retail database: {db.info['game']}, built {db.info['built']}", ""]
    changes = collections.defaultdict(dict)      # lower path -> {arc rel: (case path, data, added)}
    kept_retail = collections.defaultdict(list)  # lower path -> mod arcs that keep the retail version
    arc_dirs = {}                                # arc rel -> folder for arc-only files

    for info in z.infolist():
        if info.is_dir():
            continue
        zp = info.filename
        if zp.lower().endswith(".arc"):
            rel = db.find_arc(zp)
            data = z.read(info)
            if rel is None:
                files[f"arc/DX9/{PurePosixPath(zp).name}"] = data
                report.append(f"SIDE-LOADED ARC  arc/DX9/{PurePosixPath(zp).name}  (the game has no arc of this name)")
                continue
            inner = PurePosixPath(rel).relative_to(PurePosixPath(rel).parts[0])  # arc/DX9/x.arc
            arc_dirs[rel] = str(inner.with_suffix(""))
            retail = db.arcs[rel]
            for k, (case, d) in arc_entries(data).items():
                if retail.get(k) == digest(d):
                    kept_retail[k].append(rel)
                else:
                    changes[k][rel] = (case, d, k not in retail)
        else:
            t = loose_target(zp)
            if t is not None:
                files[str(t)] = z.read(info)
                report.append(f"LOOSE {t}   (shipped by the mod as {zp})")
            elif PurePosixPath(zp).suffix.lower() in DOC_EXTS:
                files[f"_docs/{name}/{PurePosixPath(zp).name}"] = z.read(info)
            else:
                report.append(f"SKIPPED {zp}   (not an arc and not below nativePC\\ / data\\ ...)")

    if not arc_dirs and not any(l.startswith("SIDE") for l in report):
        report.append("WARNING: the zip contains no .arc files - nothing to convert")
    n_data = n_arc = 0
    for k in sorted(changes):
        versions = changes[k]
        added = [rel for rel, (_, _, a) in versions.items() if a]
        datas = {zlib.crc32(d) for _, d, _ in versions.values()}
        case = next(iter(versions.values()))[0]
        if len(datas) == 1 and not kept_retail.get(k):
            # data\ replaces the entry where the arc has it; arcs that did not have it get it added explicitly
            # (a data\ file never adds entries to arcs)
            data = next(iter(versions.values()))[1]
            if len(added) < len(versions):
                files[case] = data
                others = sorted(set(db.holders.get(k, [])) - set(versions))
                note = f"; also reaches {len(others)} retail arcs the mod did not ship" if others else ""
                report.append(f"DATA  {case}   (same new version in {len(versions)} arc(s){note})")
                n_data += 1
            for rel in sorted(added):
                files[f"{arc_dirs[rel]}/{versions[rel][0]}"] = data
                report.append(f"ARC   {arc_dirs[rel]}/{versions[rel][0]}   (added to this arc by the mod)")
                n_arc += 1
            continue
        why = []
        if added:
            why.append(f"added by the mod to {', '.join(PurePosixPath(a).stem for a in sorted(added))}")
        if len(datas) > 1:
            why.append(f"{len(datas)} different versions")
        if kept_retail.get(k):
            why.append(f"retail version kept in {', '.join(PurePosixPath(a).stem for a in kept_retail[k])}")
        for rel, (c, d, _) in sorted(versions.items()):
            files[f"{arc_dirs[rel]}/{c}"] = d
            report.append(f"ARC   {arc_dirs[rel]}/{c}   ({'; '.join(why)})")
            n_arc += 1

    summary = (f"{n_data} data\\ files, {n_arc} arc-only files, "
               f"{sum(1 for l in report if l.startswith(('LOOSE', 'SIDE')))} loose files / side-loaded arcs")
    report.insert(2, summary)
    report_path = "report.txt" if folder else f"_reports/{name}.txt"
    files[report_path] = ("\n".join(report) + "\n").encode("utf-8")
    root = f"nativePC_mod/{name}/" if folder else "nativePC_mod/"
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as oz:
        for p in sorted(files):
            oz.writestr((root + p).replace("\\", "/"), files[p])
    return summary


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mods", type=Path, nargs="*", help="mod .zip files with repacked .arc files")
    ap.add_argument("-o", "--out", type=Path, help="output .zip (one input only; default: <mod>_sideload.zip)")
    ap.add_argument("--name", help="name of the report (and of the --folder folder); one input only; default: zip name")
    ap.add_argument("--folder", action="store_true", help=r"put the files into nativePC_mod\<name>\ (a separate mod)")
    ap.add_argument("--db", type=Path, default=HERE / "data" / "re6_retail_db.json.gz")
    a = ap.parse_args()
    if not a.mods:
        print("把一个或多个 mod 的 .zip 拖到 .bat 上（或作为参数传入）。\n"
              "Drag one or more mod .zip files onto the .bat (or pass them as arguments).")
        return 2
    if len(a.mods) > 1 and (a.out or a.name):
        ap.error("--out / --name need exactly one input")
    db = RetailDB(a.db)
    failed = 0
    for mod in a.mods:
        print(f"\n== {mod.name}")
        try:
            if not zipfile.is_zipfile(mod):
                raise ValueError("不是 zip 文件 / not a zip file")
            out = a.out or mod.with_name(mod.stem + "_sideload.zip")
            print(convert(mod, out, a.name or mod.stem, db, a.folder))
            print("完成 / done ->", out)
        except Exception as ex:  # keep going with the other inputs
            failed += 1
            print(f"失败 / FAILED: {ex}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
