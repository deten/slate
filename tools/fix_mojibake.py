"""Find and repair double-encoded UTF-8 in the source tree.

Editing a UTF-8 file with a tool that decodes it as cp1252 and writes it back
as UTF-8 turns every non-ASCII character into two or three Latin-1 characters:
an em dash becomes "a EUR ..." and never recovers on its own. The damage is
invisible in a diff viewer that guesses the same wrong encoding, and it reaches
the user as garbage in a menu label.

Detection is by round trip rather than by a list of known bad sequences: text
that survives encode-to-cp1252 then decode-as-UTF-8 *and comes back different*
was double-encoded, and the result of that round trip is the repair.

    python tools/fix_mojibake.py           # report
    python tools/fix_mojibake.py --write   # repair in place
"""

from __future__ import annotations

import sys
from pathlib import Path

SUFFIXES = {".cpp", ".hpp", ".h", ".c", ".md", ".txt", ".py", ".ps1"}
SKIP_DIRS = {"build", ".tools", ".git", "prototype-ts", ".cp-images", "node_modules"}


def repair(text: str) -> str | None:
    """The un-double-encoded text, or None when it is already correct."""
    try:
        # The exact inverse of the corruption: the bytes were once cp1252 for
        # these characters, and reading them as UTF-8 recovers the original.
        fixed = text.encode("cp1252").decode("utf-8")
    except (UnicodeEncodeError, UnicodeDecodeError):
        return None
    if fixed == text:
        return None
    # Guard against a false positive: repairing must not introduce replacement
    # characters, and must actually shorten the text (two chars become one).
    if "�" in fixed or len(fixed) >= len(text):
        return None
    return fixed


def repair_document(text: str) -> str | None:
    """Repair line by line.

    Whole-file repair misses partially damaged files: one character that cp1252
    cannot represent -- a correctly encoded arrow or a diameter sign -- makes
    the round trip throw and the entire file look clean. Real files are mixed,
    because only the lines touched by the offending tool were corrupted.
    """
    lines = text.splitlines(keepends=True)
    changed = False
    for i, line in enumerate(lines):
        fixed = repair(line)
        if fixed is not None:
            lines[i] = fixed
            changed = True
    return "".join(lines) if changed else None


def main() -> int:
    write = "--write" in sys.argv
    root = Path(__file__).resolve().parent.parent

    damaged = 0
    for path in sorted(root.rglob("*")):
        if not path.is_file() or path.suffix not in SUFFIXES:
            continue
        if any(part in SKIP_DIRS for part in path.relative_to(root).parts):
            continue

        raw = path.read_bytes()
        had_bom = raw.startswith(b"\xef\xbb\xbf")
        try:
            text = raw.decode("utf-8-sig")
        except UnicodeDecodeError:
            print(f"  not UTF-8, skipped: {path.relative_to(root)}")
            continue

        fixed = repair_document(text)
        if fixed is None:
            continue

        damaged += 1
        sample = next(
            (line.strip() for line in text.splitlines() if repair(line + "\n")), ""
        )
        print(f"  {path.relative_to(root)}")
        if sample:
            print(f"      {sample[:96]}")

        if write:
            # Written without a BOM. A BOM is what let the corruption happen in
            # the first place -- a BOM-less UTF-8 file read by a tool that
            # assumes the system codepage is the whole failure mode -- but
            # adding one only moves the problem, and the compiler is told the
            # encoding explicitly by /utf-8.
            path.write_bytes(fixed.encode("utf-8"))
        elif had_bom:
            pass

    if damaged == 0:
        print("No double-encoded files found.")
    else:
        print(f"\n{damaged} file(s) {'repaired' if write else 'affected'}.")
        if not write:
            print("Re-run with --write to repair.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
