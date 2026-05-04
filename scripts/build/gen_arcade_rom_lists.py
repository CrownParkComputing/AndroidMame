#!/usr/bin/env python3
"""Generate ROM/CHD lists for the current arcade-only subtarget.

This reads src/mame/arcade.flt, parses only the included source files, and
applies the same launcher-visible filtering rules used by the Android build.
"""

from __future__ import annotations

import argparse
import csv
import re
from pathlib import Path


MACRO_START = re.compile(r"\b(GAME|GAMEL|GAME_CUSTOM)\s*\(")
ROM_START_RE = re.compile(r"\bROM_START\s*\(\s*([A-Za-z0-9_]+)\s*\)")
BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)
LINE_COMMENT_RE = re.compile(r"//[^\n]*")
QUOTE_RE = re.compile(r'"([^"]*)"')
YEAR_RE = re.compile(r"^[0-9?]{4}$")
SHORTNAME_RE = re.compile(r"^[A-Za-z0-9_]+$")
BLOCKED_SUBSTRINGS = ("pokr", "poker", "slot", "fruit", "casino", "jackpot")
BLOCKED_TEXT_TOKENS = ("slot", "fruit", "casino", "jackpot", "keno", "lotto", "bingo")
BLOCKED_PREFIXES = ("m5", "mpu", "sc", "jp")
HIDDEN_FLAGS = ("MACHINE_IS_BIOS_ROOT", "MACHINE_IS_DEVICE", "MACHINE_NO_STANDALONE")
DISK_TOKENS = ("DISK_REGION", "DISK_IMAGE", "DISK_IMAGE_READONLY", "DISK_IMAGE_READONLY_OPTIONAL")


class GameEntry(dict):
    pass


def strip_comments(content: str) -> str:
    content = BLOCK_COMMENT_RE.sub("", content)
    return LINE_COMMENT_RE.sub("", content)


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


def normalize_string(value: str) -> str:
    value = value.strip()
    if value.startswith('"') and value.endswith('"') and len(value) >= 2:
        value = value[1:-1]
    return value.replace("|", " ").strip()


def is_launcher_allowed(shortname: str, parent: str, macro_text: str) -> bool:
    lower = shortname.lower()
    if lower.startswith(BLOCKED_PREFIXES):
        return False
    if any(part in lower for part in BLOCKED_SUBSTRINGS):
        return False
    if any(flag in macro_text for flag in HIDDEN_FLAGS):
        return False
    if parent.lower().endswith("bios"):
        return False
    return True


def is_export_allowed(game: GameEntry) -> bool:
    if not game["allowed"]:
        return False
    text = " ".join(
        (
            game["shortname"],
            game["manufacturer"],
            game["description"],
        )
    ).lower()
    return not any(token in text for token in BLOCKED_TEXT_TOKENS)


def parse_chd_blocks(content: str) -> dict[str, bool]:
    result: dict[str, bool] = {}
    matches = list(ROM_START_RE.finditer(content))
    for index, match in enumerate(matches):
        start = match.end()
        end = matches[index + 1].start() if index + 1 < len(matches) else len(content)
        block = content[start:end]
        rom_name = match.group(1).lower()
        result[rom_name] = any(token in block for token in DISK_TOKENS)
    return result


def parse_games_from_file(source_path: Path) -> tuple[dict[str, GameEntry], dict[str, bool]]:
    raw = source_path.read_text(encoding="utf-8", errors="replace")
    content = strip_comments(raw)
    games: dict[str, GameEntry] = {}
    chd_by_rom = parse_chd_blocks(content)

    for match in MACRO_START.finditer(content):
        args_str = extract_macro_args(content, match)
        if args_str is None:
            continue
        args = split_args(args_str)
        if len(args) < 10:
            continue

        year = normalize_string(args[0])
        shortname = args[1].strip().lower()
        parent = args[2].strip().lower()
        quotes = QUOTE_RE.findall(args_str)
        if len(quotes) < 2:
            continue
        manufacturer = normalize_string(quotes[-2])
        description = normalize_string(quotes[-1])

        if not YEAR_RE.match(year):
            continue
        if not SHORTNAME_RE.match(shortname):
            continue
        if parent == "0":
            parent = ""

        games[shortname] = GameEntry(
            shortname=shortname,
            parent=parent,
            year=year,
            manufacturer=manufacturer,
            description=description,
            source=source_path,
            allowed=is_launcher_allowed(shortname, parent, args_str),
            own_chd=chd_by_rom.get(shortname, False),
        )

    return games, chd_by_rom


def requires_chd(shortname: str, games: dict[str, GameEntry], trail: set[str] | None = None) -> bool:
    if trail is None:
        trail = set()
    if shortname in trail:
        return False
    trail.add(shortname)

    game = games.get(shortname)
    if not game:
        return False
    if game["own_chd"]:
        return True
    parent = game["parent"]
    if parent:
        return requires_chd(parent, games, trail)
    return False


def load_filter_list(filter_path: Path) -> list[Path]:
    source_root = filter_path.parent
    entries: list[Path] = []
    for line in filter_path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        entries.append(source_root / line)
    return entries


def generate_lists(root_dir: Path, output_dir: Path) -> tuple[Path, Path, Path, int, int]:
    filter_path = root_dir / "src" / "mame" / "arcade.flt"
    source_files = load_filter_list(filter_path)

    games: dict[str, GameEntry] = {}
    for source_path in source_files:
        parsed_games, _ = parse_games_from_file(source_path)
        games.update(parsed_games)

    visible = []
    chd_required = []
    for shortname in sorted(games):
        game = games[shortname]
        if not is_export_allowed(game):
            continue
        chd_needed = requires_chd(shortname, games)
        row = {
            "shortname": shortname,
            "parent": game["parent"],
            "year": game["year"],
            "manufacturer": game["manufacturer"],
            "description": game["description"],
            "chd_required": "yes" if chd_needed else "no",
        }
        visible.append(row)
        if chd_needed:
            chd_required.append(shortname)

    output_dir.mkdir(parents=True, exist_ok=True)
    csv_path = output_dir / "arcade_supported_sets.csv"
    supported_txt_path = output_dir / "arcade_supported_sets.txt"
    txt_path = output_dir / "arcade_chd_required_sets.txt"

    with csv_path.open("w", encoding="utf-8", newline="") as csv_file:
        writer = csv.DictWriter(
            csv_file,
            fieldnames=["shortname", "parent", "year", "manufacturer", "description", "chd_required"],
        )
        writer.writeheader()
        writer.writerows(visible)

    with supported_txt_path.open("w", encoding="utf-8") as txt_file:
        for row in visible:
            txt_file.write(f"{row['shortname']}\n")

    with txt_path.open("w", encoding="utf-8") as txt_file:
        for shortname in chd_required:
            txt_file.write(f"{shortname}\n")

    return csv_path, supported_txt_path, txt_path, len(visible), len(chd_required)


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate arcade ROM and CHD lists from arcade.flt")
    parser.add_argument("--root", default=Path(__file__).resolve().parents[2], type=Path)
    parser.add_argument("--output-dir", default=None)
    args = parser.parse_args()

    root_dir = args.root.resolve()
    output_dir = Path(args.output_dir) if args.output_dir else (root_dir / "android-project" / "app" / "src" / "main" / "assets")
    csv_path, supported_txt_path, txt_path, visible_count, chd_count = generate_lists(root_dir, output_dir)
    print(f"Wrote {visible_count} visible arcade sets to {csv_path}")
    print(f"Wrote {visible_count} supported shortnames to {supported_txt_path}")
    print(f"Wrote {chd_count} CHD-required sets to {txt_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
