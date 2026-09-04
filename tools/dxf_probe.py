"""Dump selected DXF records, for diagnosing a specific reading problem.

A general-purpose group-pair viewer. The existing round-trip checker counts
entities; this one shows what is actually in them, which is what you need when
a drawing reads but looks wrong.

    python tools/dxf_probe.py file.dxf VIEWPORT --where 69=2 --limit 1
    python tools/dxf_probe.py file.dxf MTEXT --codes 1,50,11,21,71 --limit 8
"""

from __future__ import annotations

import argparse
import io
from pathlib import Path


def records(path: Path):
    """Yield (type, [(code, value), ...]) for every record in the file."""
    with io.open(path, encoding="utf-8", errors="replace") as handle:
        lines = [line.rstrip("\r\n") for line in handle]

    current_type = None
    pairs: list[tuple[int, str]] = []
    for i in range(0, len(lines) - 1, 2):
        try:
            code = int(lines[i].strip())
        except ValueError:
            continue
        value = lines[i + 1].strip()
        if code == 0:
            if current_type is not None:
                yield current_type, pairs
            current_type = value
            pairs = []
        else:
            pairs.append((code, value))
    if current_type is not None:
        yield current_type, pairs


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("file")
    parser.add_argument("type", help="record type, e.g. VIEWPORT")
    parser.add_argument("--codes", help="comma-separated codes to show")
    parser.add_argument("--where", action="append", default=[],
                        help="code=value filter, repeatable")
    parser.add_argument("--limit", type=int, default=3)
    args = parser.parse_args()

    wanted = None
    if args.codes:
        wanted = {int(c) for c in args.codes.split(",")}
    filters = [(int(f.split("=")[0]), f.split("=", 1)[1]) for f in args.where]

    shown = 0
    for record_type, pairs in records(Path(args.file)):
        if record_type.upper() != args.type.upper():
            continue
        if any(all(str(v).strip() != value for c, v in pairs if c == code)
               for code, value in filters):
            continue

        print(f"--- {record_type} ---")
        for code, value in pairs:
            if wanted is None or code in wanted:
                print(f"  {code:>4} = {value}")
        shown += 1
        if shown >= args.limit:
            break

    if shown == 0:
        print(f"no {args.type} records matched")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
