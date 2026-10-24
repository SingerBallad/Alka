#!/usr/bin/env python3
import pathlib
import shutil
import sys

GOLDEN_DIR = pathlib.Path(__file__).resolve().parent.parent / "tests" / "Ts" / "Golden"


def main() -> int:
    updated = 0
    for actual in sorted(GOLDEN_DIR.glob("*.actual.d.ts")):
        golden = actual.with_name(actual.name.replace(".actual.d.ts", ".d.ts"))
        shutil.copyfile(actual, golden)
        print(f"updated {golden.name}")
        updated += 1
    if updated == 0:
        print("no .actual.d.ts files found; run the AlkaTests target first", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
