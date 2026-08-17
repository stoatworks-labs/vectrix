#!/usr/bin/env python3
"""Every preset row must carry a full set of values.

`Vectrix.cpp` already static_asserts that the preset *parameter list* and
`presets::kParamCount` agree. What no compiler catches is a preset **row** with
too few values: C++ aggregate initialisation fills the rest with zero, without a
word, so the row is accepted and silently means something else -- and because the
missing values land at the *end* of the row, the symptom is that the tube
settings of one preset are wrong while every other preset is fine.

That is a fifteen-minute bug to find by eye and a one-second one to find here.
"""

import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent.parent
SOURCE = HERE / "source" / "Presets.h"


def main() -> int:
    text = SOURCE.read_text()

    start = text.index("enum Param\n{")
    end = text.index("kParamCount", start)
    names = [
        line.strip().rstrip(",")
        for line in text[start:end].splitlines()
        if line.strip().startswith("k")
    ]
    expected = len(names)

    if expected == 0:
        print("could not parse the Param enum out of Presets.h", file=sys.stderr)
        return 1

    failures = 0
    rows = 0

    for match in re.finditer(r'\{\s*"([^"]+)",\s*\{(.*?)\}\s*\}', text, re.S):
        name, body = match.group(1), match.group(2)
        rows += 1

        # Strip trailing line comments before splitting -- the table carries a
        # column-header comment that is full of commas.
        cleaned = "\n".join(line.split("//")[0] for line in body.splitlines())
        values = [v for v in cleaned.split(",") if v.strip()]

        if len(values) != expected:
            print(f"FAIL  {name}: {len(values)} values, expected {expected}")
            failures += 1

    if rows == 0:
        print("no preset rows found -- has the table's shape changed?", file=sys.stderr)
        return 1

    if failures:
        print(f"\n{failures} of {rows} preset rows are the wrong width")
        return 1

    print(f"ok    {rows} preset rows, {expected} values each")
    return 0


if __name__ == "__main__":
    sys.exit(main())
