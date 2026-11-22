#!/usr/bin/env python3
"""Regenerate the C headers for the standard Cyphal data types the kit's node uses, in cyphal/dsdl/.

The headers are generated here by Nunavut and committed, so that a firmware's build (and a clean
checkout) needs no Python packages. Run this after adding a type to TYPES or after moving the DSDL
submodule:

    tools/gen_dsdl.py [--nnvg PATH] [--check]

What the committed headers were generated with:

    nunavut   2.3.1 (`nnvg`)
    DSDL      the uavcan root namespace, OpenCyphal/public_regulated_data_types
              a229bb78e76c48a3082be163bc240b2c13ff2d89 ("Extend uavcan si angle and length types
              (#150)", 2025-08-03), at third_party/public_regulated_data_types

The script refuses another DSDL revision or nunavut version unless told otherwise
(--any-revision, --any-nunavut), so that the headers, and the tests that compile the same tree for
pycyphal, cannot drift apart unseen.

Only the uavcan root is here. An application with types of its own (holo-player-fw's r2 root)
generates those in its own tree, with this submodule as the lookup directory, and checks that its
DSDL revision is this one.

Only the types in TYPES, the headers they include, and Nunavut's serialization support header are
copied out. Two things nnvg writes into every file are taken out or fixed so that the same inputs
give the same bytes: the time and machine of the run, and the absolute path of the source file. The
path is written as SOURCE_LABEL, the path the set was first generated under (in holo-player-fw,
whose copy this is), so that the kit's set is byte for byte the one that firmware committed: one
generated set, and a diff of the two says so. It names no directory of the kit's.

--check generates into a temporary directory and compares with the committed headers, changing
nothing: exit status 1 and the differing files listed when they differ.

Needs nunavut (`pip install nunavut==2.3.1`); nothing else.
"""

from __future__ import annotations

import argparse
import filecmp
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
UAVCAN = ROOT / "third_party" / "public_regulated_data_types"
OUT = ROOT / "cyphal" / "dsdl"

DSDL_REVISION = "a229bb78e76c48a3082be163bc240b2c13ff2d89"
NUNAVUT_VERSION = "2.3.1"
SOURCE_LABEL = "external/public_regulated_data_types"

# The types the node uses, and those holo-player-fw's ports use from the standard set; their
# dependencies follow from the #include lines. The node zero-initialises these structs rather than
# calling Nunavut's generated <type>_initialize_(): that deserializes a one-byte buffer, and GCC 15
# raises false array-bounds errors in it (-Werror makes them fatal).
TYPES = [
    "uavcan/node/Heartbeat_1_0.h",
    "uavcan/node/GetInfo_1_0.h",
    "uavcan/node/ExecuteCommand_1_3.h",
    "uavcan/node/port/List_1_0.h",
    "uavcan/pnp/NodeIDAllocationData_1_0.h",
    "uavcan/_register/Access_1_0.h",     # `register` is C's: Nunavut's directory and names are _register
    "uavcan/_register/List_1_0.h",
    "uavcan/file/List_0_2.h",
    "uavcan/file/GetInfo_0_2.h",
]
SUPPORT = "nunavut/support/serialization.h"

INCLUDE_RE = re.compile(r"^#include <([^>]+)>", re.M)
# The run's time and machine: "// Generated at: ..." and the "// Platform" block under it
VOLATILE_RE = re.compile(r"^// Generated at:.*\n|^// Platform\n(?://     \S.*\n)+//\n", re.M)


def run(argv: list[str]) -> str:
    return subprocess.run(argv, check=True, capture_output=True, text=True).stdout.strip()


def check_revision(path: Path, want: str, any_revision: bool) -> str:
    if not path.is_dir() or not any(path.iterdir()):
        sys.exit(f"no DSDL at {path.relative_to(ROOT)}: git submodule update --init {path.relative_to(ROOT)}")
    revision = run(["git", "-C", str(path), "rev-parse", "HEAD"])
    if revision != want and not any_revision:
        sys.exit(f"public_regulated_data_types is at {revision}, not {want} "
                 f"(--any-revision to go on, then update this script)")
    return revision


def generate(nnvg: str, out: Path) -> int:
    """The wanted headers into `out` (which must not exist); how many"""
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run([nnvg, "--target-language", "c", "--language-standard", "c11", "--outdir", tmp,
                        str(UAVCAN / "uavcan")], check=True)
        generated = Path(tmp)
        # A name asked for that nnvg didn't write is a mistake (a typo, a type moved or renamed),
        # found before anything is deleted. Only a name from an #include line may be another
        # header (<stdint.h> and the like), and is skipped.
        missing = [name for name in [*TYPES, SUPPORT] if not (generated / name).is_file()]
        if missing:
            sys.exit(f"nnvg wrote no {', '.join(missing)}: check TYPES and SUPPORT in this script "
                     f"({OUT.relative_to(ROOT)} left as it was)")
        wanted, todo = set(), [*TYPES, SUPPORT]
        while todo:
            name = todo.pop()
            if name in wanted:
                continue
            wanted.add(name)
            todo.extend(include for include in INCLUDE_RE.findall((generated / name).read_text())
                        if (generated / include).is_file())
        prefix = f"{UAVCAN}/"
        for name in sorted(wanted):
            text = VOLATILE_RE.sub("", (generated / name).read_text()).replace(prefix, f"{SOURCE_LABEL}/")
            dest = out / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_text(text)
        return len(wanted)


def differences(a: Path, b: Path) -> list[str]:
    """Files in one tree and not the other, or in both and different, relative to the trees"""
    out: list[str] = []

    def walk(cmp: filecmp.dircmp, rel: Path) -> None:
        out.extend(str(rel / n) for n in cmp.left_only + cmp.right_only + cmp.funny_files)
        _, mismatch, errors = filecmp.cmpfiles(cmp.left, cmp.right, cmp.common_files, shallow=False)
        out.extend(str(rel / n) for n in mismatch + errors)
        for name, sub in cmp.subdirs.items():
            walk(sub, rel / name)

    walk(filecmp.dircmp(a, b), Path())
    return sorted(out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--nnvg", default=shutil.which("nnvg") or "nnvg", help="the nnvg to run (default: from PATH)")
    parser.add_argument("--check", action="store_true", help="compare with the committed headers, changing nothing")
    parser.add_argument("--any-revision", action="store_true", help="accept a DSDL checkout at another revision")
    parser.add_argument("--any-nunavut", action="store_true", help=f"accept a nunavut other than {NUNAVUT_VERSION}")
    args = parser.parse_args()

    revision = check_revision(UAVCAN, DSDL_REVISION, args.any_revision)
    version = run([args.nnvg, "--version"])
    if version != NUNAVUT_VERSION and not args.any_nunavut:
        sys.exit(f"nnvg is {version}, not {NUNAVUT_VERSION} (--any-nunavut to go on, then update this script)")

    if args.check:
        with tempfile.TemporaryDirectory() as tmp:
            fresh = Path(tmp) / "dsdl"
            count = generate(args.nnvg, fresh)
            diff = differences(fresh, OUT)
        if diff:
            print(f"{OUT.relative_to(ROOT)} differs from what DSDL {revision[:7]} and nunavut {version} give:")
            for name in diff:
                print(f"  {name}")
            return 1
        print(f"{OUT.relative_to(ROOT)}: {count} headers, as DSDL {revision[:7]} and nunavut {version} give")
        return 0

    if OUT.exists():
        shutil.rmtree(OUT)
    count = generate(args.nnvg, OUT)
    for path in sorted(OUT.rglob("*.h")):
        print(f"  {path.relative_to(ROOT)}")
    print(f"{count} headers from DSDL {revision[:7]}, nunavut {version}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
