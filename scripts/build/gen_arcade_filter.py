#!/usr/bin/env python3
"""Generate an arcade-focused MAME subtarget filter.

The Android launcher already applies a coarse arcade-only policy based on
generated metadata. This script mirrors those rules at the source-file level
so native Android builds can exclude most non-arcade driver projects.
"""

from __future__ import annotations

import argparse
import glob
import os
import re
from pathlib import Path


MACRO_START = re.compile(r"\b(GAME|GAMEL|GAME_CUSTOM)\s*\(")
NON_ARCADE_MACRO = re.compile(r"\b(CONS|COMP|SYST)\s*\(")
SHORTNAME_RE = re.compile(r"^[A-Za-z0-9_]+$")
YEAR_RE = re.compile(r"^[0-9?]{4}$")
BLOCKED_SUBSTRINGS = ("pokr", "poker", "slot", "fruit", "casino", "jackpot")
BLOCKED_PREFIXES = ("m5", "mpu", "sc", "jp")
HIDDEN_FLAGS = ("MACHINE_IS_BIOS_ROOT", "MACHINE_IS_DEVICE", "MACHINE_NO_STANDALONE")
BLOCKED_TOPLEVEL_DIRS = {
    "acorn",
    "alesis",
    "altos",
    "ampex",
    "ampro",
    "apple",
    "aristocrat",
    "att",
    "ausnz",
    "banctec",
    "beehive",
    "bondwell",
    "bmc",
    "brother",
    "burroughs",
    "barcrest",
    "bfm",
    "camputers",
    "canon",
    "cantab",
    "chromatics",
    "citoh",
    "commodore",
    "compugraphic",
    "comx",
    "concept",
    "conitec",
    "cromemco",
    "cybiko",
    "dai",
    "dec",
    "dg",
    "dgrm",
    "elektor",
    "elektron",
    "emusys",
    "enterprise",
    "entex",
    "epson",
    "ericsson",
    "esprit",
    "facit",
    "fairchild",
    "fairlight",
    "fujitsu",
    "funtech",
    "funworld",
    "handheld",
    "hds",
    "heathzenith",
    "hec2hrp",
    "hitachi",
    "homelab",
    "hominn",
    "hp",
    "husky",
    "ibm",
    "intel",
    "intergraph",
    "interton",
    "isc",
    "kaypro",
    "koei",
    "kontron",
    "korg",
    "kurzweil",
    "kyocera",
    "leapfrog",
    "learnsiegler",
    "liberty",
    "linn",
    "lsi",
    "luxor",
    "makerbot",
    "matsushita",
    "maygay",
    "memotech",
    "mera",
    "microcraft",
    "microkey",
    "microsoft",
    "microterm",
    "mits",
    "mitsubishi",
    "morrow",
    "motorola",
    "msx",
    "multitech",
    "mupid",
    "nakajima",
    "nasco",
    "nascom",
    "natsemi",
    "ncd",
    "ncr",
    "nec",
    "netronics",
    "next",
    "nix",
    "nokia",
    "northstar",
    "novation",
    "oberheim",
    "olivetti",
    "omnibyte",
    "omron",
    "openuni",
    "orla",
    "osborne",
    "osi",
    "paia",
    "palm",
    "pc",
    "pce",
    "philips",
    "pitronics",
    "poly88",
    "positron",
    "psion",
    "quantel",
    "qume",
    "ramtek",
    "rca",
    "regnecentralen",
    "robotron",
    "rockwell",
    "roland",
    "rolm",
    "sage",
    "saitek",
    "samcoupe",
    "samsung",
    "sanyo",
    "sequential",
    "sgi",
    "sharp",
    "siemens",
    "sinclair",
    "skeleton",
    "slicer",
    "sony",
    "sord",
    "sun",
    "svi",
    "svision",
    "swtpc",
    "synertek",
    "tab",
    "tandberg",
    "tangerine",
    "tatung",
    "tektronix",
    "telenova",
    "televideo",
    "tesla",
    "thomson",
    "ti",
    "tigertel",
    "tiki",
    "tomy",
    "torch",
    "toshiba",
    "trainer",
    "trs",
    "tryom",
    "tvgames",
    "ultimachine",
    "ultratec",
    "umc",
    "unicard",
    "unisonic",
    "unisys",
    "usp",
    "ussr",
    "vectorgraphic",
    "verifone",
    "vidbrain",
    "videoton",
    "virtual",
    "visual",
    "votrax",
    "vtech",
    "wang",
    "wavemate",
    "westinghouse",
    "wicat",
    "wing",
    "wyse",
    "xerox",
    "yamaha",
    "yeno",
}
BLOCKED_PATH_PARTS = (
    "bingo",
    "card",
    "jackpot",
    "keno",
    "lotto",
    "pinball/",
    "fruit",
    "casino",
    "slots",
    "poker",
)


def split_args(arg_string: str) -> list[str]:
    args: list[str] = []
    depth = 0
    in_string = False
    current: list[str] = []
    index = 0
    while index < len(arg_string):
        char = arg_string[index]
        if in_string:
            current.append(char)
            if char == "\\" and index + 1 < len(arg_string):
                index += 1
                current.append(arg_string[index])
            elif char == '"':
                in_string = False
        elif char == '"':
            in_string = True
            current.append(char)
        elif char == '(':
            depth += 1
            current.append(char)
        elif char == ')':
            depth -= 1
            current.append(char)
        elif char == ',' and depth == 0:
            args.append("".join(current))
            current = []
        else:
            current.append(char)
        index += 1
    if current:
        args.append("".join(current))
    return args


def extract_macro_args(content: str, match: re.Match[str]) -> str | None:
    depth = 1
    position = match.end()
    in_string = False
    while position < len(content) and depth > 0:
        char = content[position]
        if in_string:
            if char == "\\" and position + 1 < len(content):
                position += 2
                continue
            if char == '"':
                in_string = False
        elif char == '"':
            in_string = True
        elif char == '(':
            depth += 1
        elif char == ')':
            depth -= 1
        position += 1
    if depth != 0:
        return None
    return content[match.end():position - 1]


def should_include(shortname: str, parent: str, macro_text: str) -> bool:
    lower = shortname.lower()
    if lower.startswith(BLOCKED_PREFIXES):
        return False
    if any(part in lower for part in BLOCKED_SUBSTRINGS):
        return False
    if parent.lower().endswith("bios"):
        return False
    if any(flag in macro_text for flag in HIDDEN_FLAGS):
        return False
    return True


def is_blocked_source(rel_path: Path) -> bool:
    rel_posix = rel_path.as_posix().lower()
    if any(part in rel_posix for part in BLOCKED_PATH_PARTS):
        return True
    topdir = rel_path.parts[0].lower() if rel_path.parts else ""
    return topdir in BLOCKED_TOPLEVEL_DIRS


def include_companion_sources(source_dir: Path, source_files: set[str]) -> set[str]:
    expanded = set(source_files)
    pending = list(source_files)

    while pending:
        rel_source = pending.pop()
        rel_path = Path(rel_source)
        stem_prefix = f"{rel_path.stem}_"
        abs_parent = source_dir / rel_path.parent

        try:
            siblings = abs_parent.iterdir()
        except OSError:
            continue

        for sibling in siblings:
            if sibling.suffix != ".cpp":
                continue

            sibling_rel = sibling.relative_to(source_dir)
            if sibling_rel == rel_path or is_blocked_source(sibling_rel):
                continue

            if sibling.stem.startswith(stem_prefix):
                sibling_rel_posix = sibling_rel.as_posix()
                if sibling_rel_posix not in expanded:
                    expanded.add(sibling_rel_posix)
                    pending.append(sibling_rel_posix)

    return expanded


def scan_sources(root_dir: Path) -> list[str]:
    source_dir = root_dir / "src" / "mame"
    source_files: set[str] = set()
    pattern = str(source_dir / "**" / "*.cpp")
    for cpp_file in glob.glob(pattern, recursive=True):
        cpp_path = Path(cpp_file)
        try:
            content = cpp_path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue

        rel_path = cpp_path.relative_to(source_dir)
        if is_blocked_source(rel_path):
            continue

        include_file = False
        for match in MACRO_START.finditer(content):
            args_str = extract_macro_args(content, match)
            if args_str is None:
                continue
            args_str = re.sub(r"/\*.*?\*/", "", args_str, flags=re.DOTALL)
            args_str = re.sub(r"//[^\n]*", "", args_str)
            args = split_args(args_str)
            if len(args) < 3:
                continue

            year = args[0].strip().strip('"')
            shortname = args[1].strip()
            parent = args[2].strip()
            if not YEAR_RE.match(year):
                continue
            if not SHORTNAME_RE.match(shortname):
                continue
            if parent == "0":
                parent = ""

            if should_include(shortname, parent, args_str):
                include_file = True
                break

        if not include_file and NON_ARCADE_MACRO.search(content):
            continue

        if include_file:
            source_files.add(rel_path.as_posix())

    return sorted(include_companion_sources(source_dir, source_files))


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate src/mame/arcade.flt")
    parser.add_argument("--root", default=Path(__file__).resolve().parents[2], type=Path)
    parser.add_argument("--output", default=None)
    args = parser.parse_args()

    root_dir = args.root.resolve()
    output_path = Path(args.output) if args.output else (root_dir / "src" / "mame" / "arcade.flt")
    sources = scan_sources(root_dir)
    output_path.write_text("\n".join(sources) + "\n", encoding="utf-8")
    print(f"Wrote {len(sources)} source entries to {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
