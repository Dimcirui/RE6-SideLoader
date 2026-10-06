r"""Build the converter package: a folder (and .zip) that holds only scripts and data, no Python and no binaries.

usage: python tools/make_converter_package.py [out dir] [--python <python-X.Y.Z-embed-amd64.zip>]

    RE6_SideloadConverter\
      Convert - drop mod zip here.bat     drag one or more mod .zip files onto it (uses the Python installed on the PC)
      README.txt
      app\arc_mod_to_loose.py, app\re6arc\arc.py, app\data\re6_retail_db.json

Upload scanners reject archives that hold other archives or executables (python.org's embeddable Python is
python.exe, DLLs and python314.zip), so the default package needs the user to install Python 3.10+ themselves.

--python <embed zip> builds the self-contained variant for GitHub releases instead: the official embeddable
Python, unmodified, is unpacked into runtime\ and the .bat prefers it over an installed Python.
"""
import argparse
import gzip
import shutil
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
NAME = "RE6_SideloadConverter"

BAT = r"""@echo off
rem RE6 Sideload Converter - drag one or more mod .zip files onto this file
setlocal
set "PYTHONUTF8=1"
set "PYTHONIOENCODING=utf-8"
chcp 65001 >nul
set "PY="
if exist "%~dp0runtime\python.exe" set PY="%~dp0runtime\python.exe"
if defined PY goto run
py -3 -c "import sys; sys.exit(sys.version_info < (3, 10))" >nul 2>&1 && set "PY=py -3"
if not defined PY python -c "import sys; sys.exit(sys.version_info < (3, 10))" >nul 2>&1 && set "PY=python"
if not defined PY goto nopython
:run
%PY% -I -B "%~dp0app\arc_mod_to_loose.py" %*
goto done
:nopython
echo.
echo 没有找到 Python 3.10 或更高版本。请从 https://www.python.org/downloads/ 安装，
echo 安装时勾选 "Add python.exe to PATH"，然后重新运行。
echo Python 3.10 or newer was not found. Install it from https://www.python.org/downloads/ ,
echo tick "Add python.exe to PATH" in the installer, then run this again.
:done
echo.
pause
"""

README = """RE6 Sideload Converter
======================

把"重新封包 arc、替换原文件"类型的 RE6 mod（.zip）转换成侧载 mod。
Turns a Resident Evil 6 mod that ships repacked .arc files (.zip) into a side-loader mod.

需要 / Requirements
  Python 3.10 或更高版本（https://www.python.org/downloads/ ，安装时勾选 "Add python.exe to PATH"）。
  不需要游戏，也不需要装其他库。
  Python 3.10 or newer (https://www.python.org/downloads/ , tick "Add python.exe to PATH"). No game install and
  no extra packages needed.

用法 / Usage
  把一个或多个 mod 的 .zip 拖到 "Convert - drop mod zip here.bat" 上。
  旁边会生成 <名字>_sideload.zip，里面是 nativePC_mod/data/...、nativePC_mod/arc/...，可以直接用 mod 管理器安装，
  或者解压到游戏目录（BH6.exe 所在的文件夹）。改了同一个文件的 mod 互相冲突，后装的覆盖先装的。
  Drag one or more mod .zip files onto "Convert - drop mod zip here.bat". It writes <name>_sideload.zip next
  to each input, laid out as nativePC_mod/data/..., nativePC_mod/arc/..., ready for a mod manager or to extract
  into the game folder. Mods that replace the same file conflict; the one installed last wins.

  游戏需要装好 RE6 Sideloader（dinput8.dll）。
  The game needs the RE6 Sideloader (dinput8.dll) next to BH6.exe.

说明 / Notes
  - app\\data\\re6_retail_db.json 是零售版条目的路径和 MD5 指纹（不包含任何游戏内容），所以不需要装游戏。
    app\\data\\re6_retail_db.json holds retail entry paths + MD5 fingerprints (no game content), which is why
    no game install is needed.
  - 只保留 mod 真正改过的文件；每个文件为什么放在那里，见 nativePC_mod/_reports/<名字>.txt。
    Only the files the mod really changed are kept; nativePC_mod/_reports/<name>.txt explains each one.
  - 命令行 / command line:
      python app\\arc_mod_to_loose.py <mod.zip> [-o out.zip] [--name Name] [--folder]
    --folder: 放进 nativePC_mod/<名字>/，作为单独的一个 mod / put the files into nativePC_mod/<name>/ as a separate mod
"""


def main():
    ap = argparse.ArgumentParser(description="Build the RE6 Sideload Converter package.")
    ap.add_argument("out_dir", nargs="?", type=Path, default=HERE.parent / "dist")
    ap.add_argument("--python", type=Path, metavar="EMBED_ZIP",
                    help="python-X.Y.Z-embed-amd64.zip from python.org: build the self-contained variant")
    a = ap.parse_args()
    out_root = a.out_dir
    name = NAME + ("_with_Python" if a.python else "")
    pkg = out_root / name
    if pkg.exists():
        shutil.rmtree(pkg)
    (pkg / "app" / "re6arc").mkdir(parents=True)
    if a.python:
        with zipfile.ZipFile(a.python) as z:
            z.extractall(pkg / "runtime")
    (pkg / "app" / "data").mkdir()
    shutil.copy(HERE / "arc_mod_to_loose.py", pkg / "app")
    for f in ("arc.py", "__init__.py"):
        shutil.copy(HERE / "re6arc" / f, pkg / "app" / "re6arc")
    shutil.copy(HERE.parent / "LICENSE", pkg / "LICENSE.txt")
    # plain .json: a .gz would be one more compressed file inside the zip for an upload scanner to object to
    with gzip.open(HERE / "data" / "re6_retail_db.json.gz") as src, \
            open(pkg / "app" / "data" / "re6_retail_db.json", "wb") as dst:
        shutil.copyfileobj(src, dst)
    (pkg / "Convert - drop mod zip here.bat").write_bytes(BAT.replace("\n", "\r\n").encode("utf-8"))
    readme = README
    if a.python:
        readme = readme.replace(
            "需要 / Requirements\n",
            "需要 / Requirements\n"
            "  【此版本自带 Python（python.org 官方嵌入版，未修改），不需要另外安装。】\n"
            "  [This build includes Python (the official embeddable one from python.org, unmodified): nothing to install.]\n")
    (pkg / "README.txt").write_bytes(readme.replace("\n", "\r\n").encode("utf-8-sig"))

    archive = out_root / f"{name}.zip"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for f in sorted(pkg.rglob("*")):
            if f.is_file():
                z.write(f, Path(name) / f.relative_to(pkg))
    size = sum(f.stat().st_size for f in pkg.rglob("*") if f.is_file())
    print(f"{pkg} ({size / 1e6:.1f} MB)\n{archive} ({archive.stat().st_size / 1e6:.1f} MB)")


if __name__ == "__main__":
    main()
