"""Sanity-check a DXF round trip from outside the test suite.

Reads a DXF, counts what is in it, and reports. Run before and after a
save from the application to confirm nothing was lost:

    python tools/roundtrip_check.py samples/sample.dxf
    python tools/roundtrip_check.py samples/saved.dxf

This is a deliberately independent implementation — a trivial group-code
counter that shares no code with the C++ reader — so it cannot agree with the
reader by sharing the same bug.
"""

from __future__ import annotations

import sys
from collections import Counter
from pathlib import Path


def read_groups(text: str) -> list[tuple[int, str]]:
    """Split DXF text into (code, value) pairs, tolerating any line ending."""
    lines = text.replace("\r\n", "\n").replace("\r", "\n").split("\n")
    groups: list[tuple[int, str]] = []
    for i in range(0, len(lines) - 1, 2):
        try:
            code = int(lines[i].strip())
        except ValueError:
            continue
        groups.append((code, lines[i + 1]))
    return groups


def summarize(path: Path) -> None:
    groups = read_groups(path.read_text(encoding="utf-8", errors="replace"))

    section = ""
    entities: Counter[str] = Counter()
    layers: list[str] = []
    linetypes: list[str] = []
    blocks: list[str] = []
    in_tables_of = ""

    for index, (code, value) in enumerate(groups):
        if code == 2 and index > 0 and groups[index - 1] == (0, "SECTION"):
            section = value
            continue

        if code == 0:
            if value == "TABLE":
                in_tables_of = ""
            elif value in ("LAYER", "LTYPE", "STYLE") and section == "TABLES":
                in_tables_of = value
            elif value == "BLOCK":
                in_tables_of = "BLOCK"
            elif section == "ENTITIES" and value not in ("ENDSEC", "SEQEND"):
                entities[value] += 1
            elif value == "ENDSEC":
                in_tables_of = ""

        if code == 2:
            if in_tables_of == "LAYER":
                layers.append(value)
                in_tables_of = ""
            elif in_tables_of == "LTYPE":
                linetypes.append(value)
                in_tables_of = ""
            elif in_tables_of == "BLOCK":
                blocks.append(value)
                in_tables_of = ""

    print(f"{path.name}  ({path.stat().st_size} bytes, {len(groups)} groups)")
    print(f"  layers    ({len(layers)}): {', '.join(sorted(layers)) or '-'}")
    print(f"  linetypes ({len(linetypes)}): {', '.join(sorted(linetypes)) or '-'}")
    print(f"  blocks    ({len(blocks)}): {', '.join(sorted(blocks)) or '-'}")
    print(f"  entities  ({sum(entities.values())}):")
    for name, count in sorted(entities.items()):
        print(f"      {name:<14} {count}")


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    for argument in sys.argv[1:]:
        summarize(Path(argument))
        print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
