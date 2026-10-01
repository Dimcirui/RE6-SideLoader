"""Locate the Resident Evil 6 install (Steam) for the developer tools."""
import re
import sys
from pathlib import Path

GAME_FOLDER = "Resident Evil 6"


def steam_libraries():
    roots = []
    try:
        import winreg
        for hive, key in ((winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam"),
                          (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Valve\Steam")):
            try:
                with winreg.OpenKey(hive, key) as k:
                    for name in ("SteamPath", "InstallPath"):
                        try:
                            roots.append(Path(winreg.QueryValueEx(k, name)[0]))
                        except OSError:
                            pass
            except OSError:
                pass
    except ImportError:
        pass
    libs = []
    for root in roots:
        libs.append(root)
        vdf = root / "steamapps" / "libraryfolders.vdf"
        if vdf.exists():
            for m in re.finditer(r'"path"\s+"([^"]+)"', vdf.read_text(encoding="utf-8", errors="replace")):
                libs.append(Path(m.group(1).replace("\\\\", "\\")))
    return libs


def find_game(arg=None) -> Path:
    """the game folder from `arg`, else the first Steam library that has it; exits with a message otherwise"""
    cands = [Path(arg)] if arg else [lib / "steamapps" / "common" / GAME_FOLDER for lib in steam_libraries()]
    for c in cands:
        if (c / "nativePC").is_dir():
            return c
    sys.exit("Resident Evil 6 not found - pass the game folder (the one with BH6.exe and nativePC) as an argument")
