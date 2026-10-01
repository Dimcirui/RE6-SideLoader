"""Build the retail entry database used by arc_mod_to_loose.py, so converting a mod needs no game install.

usage: python tools/build_retail_db.py [game dir (default: found through Steam)] [out file]

For every arc of nativePC and nativePC_dlc it stores the arc path and, per entry, the entry path (with extension)
and the first 8 bytes of the MD5 of the DECOMPRESSED data (mod tools recompress, so compressed bytes never match).
"""
import gzip
import hashlib
import json
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from re6arc import arc  # noqa: E402
from game_dir import find_game  # noqa: E402

DEFAULT_OUT = HERE / "data" / "re6_retail_db.json.gz"


def digest(data: bytes) -> str:
    return hashlib.md5(data).hexdigest()[:16]


def main():
    game = find_game(sys.argv[1] if len(sys.argv) > 1 else None)
    out = Path(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_OUT
    exe = game / "BH6.exe"
    paths, path_index, arcs = [], {}, {}
    t0 = time.time()
    for root in ("nativePC", "nativePC_dlc"):
        for p in sorted((game / root).rglob("*.arc")):
            rel = p.relative_to(game).as_posix()
            try:
                a = arc.parse(p)
            except arc.ArcError as ex:
                print("skipped", rel, ex)
                continue
            rows = []
            with open(p, "rb") as fh:
                for e in a.entries:
                    k = e.path.lower()
                    if k not in path_index:
                        path_index[k] = len(paths)
                        paths.append(e.path)
                    rows.append([path_index[k], digest(arc.read_entry(a, e, fh))])
            arcs[rel] = rows
    db = {
        "format": 1,
        "game": "Resident Evil 6 (PC)",
        "exe_size": exe.stat().st_size if exe.exists() else 0,
        "exe_md5": hashlib.md5(exe.read_bytes()).hexdigest() if exe.exists() else "",
        "built": time.strftime("%Y-%m-%d"),
        "paths": paths,
        "arcs": arcs,
    }
    out.parent.mkdir(parents=True, exist_ok=True)
    with gzip.open(out, "wt", encoding="utf-8", compresslevel=9) as fh:
        json.dump(db, fh, separators=(",", ":"))
    n = sum(len(r) for r in arcs.values())
    print(f"{len(arcs)} arcs, {n} entries, {len(paths)} paths -> {out} ({out.stat().st_size:,} bytes, "
          f"{time.time() - t0:.0f} s)")


if __name__ == "__main__":
    main()
