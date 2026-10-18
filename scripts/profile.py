#!/usr/bin/env python3
"""Read one value from a game profile.

    scripts/profile.py <game-id> <dotted.key>

Examples:
    scripts/profile.py rio apk.package      -> com.rovio.angrybirdsrio
    scripts/profile.py rio guest.lib        -> libAngryBirdsRio.so
    scripts/profile.py rio strip.libs       -> libjs.so libadcolony.so

Lists are printed space-separated (safe for `for x in $(...)`). Missing keys exit
non-zero, so a typo in a script fails loudly instead of silently passing "".
"""
import sys
import pathlib
import tomllib

root = pathlib.Path(__file__).resolve().parent.parent


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    game, key = sys.argv[1], sys.argv[2]

    path = root / "games" / game / "profile.toml"
    if not path.exists():
        print(f"profile: no such game: {path}", file=sys.stderr)
        return 2

    with open(path, "rb") as fh:
        node = tomllib.load(fh)

    for part in key.split("."):
        if not isinstance(node, dict) or part not in node:
            print(f"profile: {path}: no key {key!r}", file=sys.stderr)
            return 2
        node = node[part]

    if isinstance(node, list):
        print(" ".join(str(x) for x in node))
    elif isinstance(node, bool):
        print("true" if node else "false")
    else:
        print(node)
    return 0


if __name__ == "__main__":
    sys.exit(main())
