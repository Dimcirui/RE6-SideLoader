"""End-to-end test of build/dinput8.dll with build/test_host.exe standing in for BH6.exe.

Builds a fake game folder (copies of one retail arc + mod folders), lets the host open files through its kernel32
imports / call its copy of the rArchive entry filter, and checks the results with re6arc (tools/re6arc).

usage: python test/run_test.py [game dir] [work dir]
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
BUILD = HERE.parent / "build"
sys.path.insert(0, str(HERE.parent / "tools"))
from re6arc import arc  # noqa: E402
from game_dir import find_game  # noqa: E402

GAME = find_game(sys.argv[1] if len(sys.argv) > 1 else None)
WORK = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(os.environ.get("TEMP", ".")) / "re6_sideloader_test"
REL = Path("arc/DX9/MenuAda.arc")  # inside nativePC

failures = 0


def check(cond, what):
    global failures
    print(("ok   " if cond else "FAIL ") + what)
    failures += not cond


def host(*args):
    r = subprocess.run([str(WORK / "test_host.exe"), *map(str, args)], cwd=WORK, capture_output=True, text=True)
    return r


def entries(path):
    a = arc.parse(path)
    with open(path, "rb") as fh:
        return {e.path.lower(): arc.read_entry(a, e, fh) for e in a.entries}, a


def log():
    return (WORK / "re6_sideloader.log").read_text(encoding="utf-8-sig")


def ini(**kw):
    (WORK / "re6_sideloader.ini").write_text("[Loader]\n" + "".join(f"{k}={v}\n" for k, v in kw.items()))


def put(rel, data):
    p = WORK / "nativePC_mod" / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(data)
    return p


def win(p):
    return str(p).replace("/", "\\")


# ---- fake game folder ---------------------------------------------------------------------------------------------
if WORK.exists():
    shutil.rmtree(WORK)
for root in ("nativePC", "nativePC_dlc"):
    (WORK / root / REL.parent).mkdir(parents=True)
    shutil.copy(GAME / "nativePC" / REL, WORK / root / REL)
shutil.copy(GAME / "nativePC" / REL, WORK / "nativePC/arc/DX9/Second.arc")
for f in ("dinput8.dll", "test_host.exe"):
    shutil.copy(BUILD / f, WORK / f)

orig, orig_arc = entries(WORK / "nativePC" / REL)
names = list(orig)
rep_arc, rep_glob, rep_root, rep_off = names[0], names[5], names[10], names[20]
stem = lambda n: n.rsplit(".", 1)[0].replace("/", chr(92))  # entry name as the engine passes it
type_glob = next(e.type_hash for e in orig_arc.entries if e.path.lower() == rep_glob)

# mods: (root) < A_base < B_mod (alphabetical, later wins); _off is disabled
put(rep_root, b"ROOT MOD")                                              # root\data\... = implicit lowest mod
put(Path("A_base") / rep_glob, b"A" * 5000)
put(Path("B_mod") / rep_glob, b"B" + os.urandom(200000))                # > 64 KiB: several stored blocks
put(Path("B_mod/arc/DX9/MenuAda") / rep_arc, b"ONLY IN MENUADA")       # arc-specific
put(Path("B_mod/arc/DX9/MenuAda/test/added_model.mod"), b"NEW" * 10)   # new entry for one arc
put(Path("B_mod/arc/DX9/MenuAda/test/Raw_Hash.276de8b7"), b"HEX")      # hash extension, mixed case
put(Path("B_mod/data/new/brand_new.tex"), b"BRAND NEW")                # in no arc at all
put(Path("B_mod/data/new/thing.276de8b7"), b"THING")                    # in no arc, extension unknown to us
put(Path("B_mod/readme.txt"), b"ignored")
put(Path("_off") / rep_off, b"DISABLED")
tiny = WORK / "tiny.arc"
arc.write_archive(tiny, [arc.Item("test/tiny", arc.hash_of_ext("tex"), 4, data=b"TINY")])
put(Path("C_side/arc/DX9/Second.arc"), tiny.read_bytes())               # side-loaded whole arc

# ---- no ini: defaults + generated file ----------------------------------------------------------------------------
print("== no ini")
r = host("open", win(Path("nativePC/data/new/brand_new.tex")), WORK / "out_d.tex")
gen = WORK / "re6_sideloader.ini"
check(gen.exists() and "Mode=engine" in gen.read_text() and "Root=nativePC_mod" in gen.read_text(),
      "missing ini is generated with the defaults")
check("filter installed: yes" in r.stdout and (WORK / "out_d.tex").read_bytes() == b"BRAND NEW",
      "without an ini the loader runs with the defaults (engine mode, nativePC_mod)")
check(gen.read_text() == (HERE.parent / "re6_sideloader.ini").read_text(), "generated ini == shipped re6_sideloader.ini")
r = host("open", win(Path("nativePC/data/new/brand_new.tex")), WORK / "out_d.tex")
check("wrote one with the default settings" not in log(), "an existing ini is not rewritten")

# ---- patch mode ---------------------------------------------------------------------------------------------------
print("== Mode=patch")
ini(Mode="patch")
out = WORK / "out_p.arc"
r = host("open", win(Path("nativePC") / REL), out, "A")
check(r.returncode == 0 and "filter installed: no" in r.stdout, "patch mode does not touch the engine filter")
got, got_arc = entries(out)
check(got[rep_arc] == b"ONLY IN MENUADA", "arc-specific folder replaces an entry")
check(got[rep_glob] == (WORK / "nativePC_mod/B_mod" / rep_glob).read_bytes(), "data\\ resource patched in, B beats A")
check(got[rep_root] == b"ROOT MOD", "root\\data\\... works as the lowest mod")
check(got[rep_off] == orig[rep_off], "_off (underscore) mod is ignored")
check(got.get("test/added_model.mod") == b"NEW" * 10, "new entry added to that arc")
check(any(e.name == "test/Raw_Hash" for e in got_arc.entries), "added entry keeps the file name case")
check(len(got) == len(orig) + 2, f"entry count {len(got)} == {len(orig)} + 2")
same = [n for n in orig if n not in (rep_arc, rep_glob, rep_root)]
check(all(got[n] == orig[n] for n in same), f"{len(same)} other entries identical")
check(f"FindFirstFileA size {out.stat().st_size}" in r.stdout, "FindFirstFile reports the patched size")
check("[(root), A_base, B_mod, C_side]" in log(), "mod list and order in the log")

r = host("open", win(WORK / "nativePC_dlc" / REL), WORK / "out_dlc.arc")
got_dlc, _ = entries(WORK / "out_dlc.arc")
check(got_dlc[rep_glob] == got[rep_glob] and got_dlc[rep_arc] == got[rep_arc], "nativePC_dlc arcs get the same mods")

r = host("open", win(Path("nativePC/arc/DX9/Second.arc")), WORK / "out_s.arc")
check((WORK / "out_s.arc").read_bytes() == tiny.read_bytes(), "side-loaded arc replaces the original")

r = host("open", win(Path("nativePC/data/new/brand_new.tex")), WORK / "out_n.tex")
check(r.returncode == 0 and (WORK / "out_n.tex").read_bytes() == b"BRAND NEW", "file that no arc has is served")
r = host("open", win(Path("resource/data/new/thing.xyz")), WORK / "out_t.bin")
check(r.returncode == 0 and (WORK / "out_t.bin").read_bytes() == b"THING", "resource\\ root + extension by stem")
# the game asks with ITS extension (rLinkUnit = .lku, 5fb399f4 = rBioSoundSequenceSe .bssq); a mod file may also
# carry the type hash as extension; two types under one name must still resolve by type
put(Path("B_mod/data/x/two.lku"), b"LINKUNIT")
put(Path("B_mod/data/x/two.5fb399f4"), b"BSSQ")
r = host("open", win(Path("nativePC/data/x/two.lku")), WORK / "out_lku.bin")
check(r.returncode == 0 and (WORK / "out_lku.bin").read_bytes() == b"LINKUNIT", "engine .lku finds the .lku file")
r = host("open", win(Path("nativePC/data/x/two.bssq")), WORK / "out_bssq.bin")
check(r.returncode == 0 and (WORK / "out_bssq.bin").read_bytes() == b"BSSQ", "engine .bssq finds the hash-named file")
r = host("open", win(Path("nativePC/data/new/missing.tex")), WORK / "out_m.bin")
check(r.returncode == 5, "missing file stays missing")

r = host("open", win(Path("nativePC") / REL), WORK / "out_c.arc")
check((WORK / "out_c.arc").read_bytes() == out.read_bytes() and "cached patch" in log(), "unchanged -> cache reused")

hot = WORK / "nativePC_mod/B_mod/arc/DX9/MenuAda" / rep_arc
r = host("reopen", win(Path("nativePC") / REL), WORK / "out_h1.arc", hot, WORK / "out_h2.arc")
h1, _ = entries(WORK / "out_h1.arc")
h2, _ = entries(WORK / "out_h2.arc")
check(h1[rep_arc] == b"ONLY IN MENUADA" and h2[rep_arc] == b"HOT RELOAD", "edit while running -> next open rebuilds")
hot.write_bytes(b"ONLY IN MENUADA")

ini(Mode="patch", Order="B_mod, A_base")
r = host("open", win(Path("nativePC") / REL), WORK / "out_o.arc")
got_o, _ = entries(WORK / "out_o.arc")
check(got_o[rep_glob] == b"A" * 5000 and got_o[rep_root] == b"ROOT MOD", "Order= changes priority (A last wins)")

# ---- engine mode --------------------------------------------------------------------------------------------------
print("== Mode=engine")
ini(Mode="engine")
r = host("filter", "arc\\DX9\\MenuAda", f"{type_glob:08x}", stem(rep_glob))
check("filter installed: yes" in r.stdout, "signature scan finds the entry filter and installs it")
check(r.returncode == 1, "filter skips an entry that has a loose resource")
check("skip " + stem(rep_glob) in log(), "skip is logged")
r = host("filter", "arc\\DX9\\MenuAda", f"{type_glob ^ 1:08x}", stem(rep_glob))
check(r.returncode == 0, "same path, other type -> kept")
type_arc = next(e.type_hash for e in orig_arc.entries if e.path.lower() == rep_arc)
put(Path("A_base") / rep_arc, b"GLOBAL")  # same path as the MenuAda-only file of B_mod
r = host("filter", "arc\\DX9\\MenuAda", f"{type_arc:08x}", stem(rep_arc))
check(r.returncode == 0, "arc-specific file keeps its entry in that arc even if a data\\ file exists")
r = host("filter", "arc\\DX9\\Second", f"{type_arc:08x}", stem(rep_arc))
check(r.returncode == 1, "... but other arcs skip it")
(WORK / "nativePC_mod/A_base" / rep_arc).unlink()
r = host("filter", "arc\\DX9\\MenuAda", f"{type_glob:08x}", stem(rep_off))
check(r.returncode == 0, "entry without loose file -> kept")

r = host("open", win(Path("nativePC") / REL), WORK / "out_e.arc")
got_e, _ = entries(WORK / "out_e.arc")
check(got_e[rep_arc] == b"ONLY IN MENUADA" and got_e[rep_glob] == orig[rep_glob],
      "engine mode only patches arc-specific folders, data\\ resources stay out of the arc")
r = host("open", win(Path("nativePC") / rep_glob), WORK / "out_l.bin")
check((WORK / "out_l.bin").read_bytes() == (WORK / "nativePC_mod/B_mod" / rep_glob).read_bytes(),
      "the skipped resource is served as a loose file from the winning mod")

shutil.rmtree(WORK / "nativePC_mod")
r = host("open", win(Path("nativePC") / REL), WORK / "out_z.arc")
check((WORK / "out_z.arc").read_bytes() == (WORK / "nativePC" / REL).read_bytes(), "no mods -> original arc untouched")

print("\n" + log())
print("FAILED" if failures else "ALL PASSED", failures)
sys.exit(1 if failures else 0)
