#!/usr/bin/env python3
"""The -Werk rename: <Function>Force becomes <Function>Werk in a repo's tracked text files, because "Force" is Akai's
device name and doesn't belong in our product names (docs/superpowers/plans/2026-10-28-werk-rename.md).

    werk_rename.py <repo>            rewrite in place, print "N lines in M files"
    werk_rename.py --check <repo>    change nothing; list what is left (path:line: text, and tracked paths with an
                                     old name in them); exit 1 if anything is, 0 if not

The eight names in three spellings each (SubForce, subforce, SUBFORCE), matched anywhere in a line, so
subforce_stages.so and SubForceStageTimes follow along; AkaiForce, "the Force" and force-shadow contain none of them.
--check matches any casing, so a stray "Subforce" shows up for a hand fix. Left alone: untracked and binary files
(a NUL in the first 8 KiB), lines with "werk-rename: keep" (the old header a plugin still reads, the "Formerly"
line), docs/superpowers/ and this script. Safe to run again: on a branch after merging a renamed main, say.
"""
import re
import subprocess
import sys

NAMES = ["Master", "Effect", "Poly", "Sub", "Drum", "Groove", "Ambient", "Rack"]
KEEP = "werk-rename: keep"
SKIP = ("docs/superpowers/", "tools/werk_rename")

PAIRS = [(old, new) for n in NAMES for old, new in
         ((n + "Force", n + "Werk"), ((n + "Force").lower(), (n + "Werk").lower()), ((n + "Force").upper(), (n + "Werk").upper()))]
RENAME = re.compile("|".join(old for old, _ in PAIRS))
NEW = dict(PAIRS)
LEFT = re.compile("|".join(n + "force" for n in NAMES), re.IGNORECASE)


def tracked(repo):
    out = subprocess.run(["git", "-C", repo, "ls-files", "-z"], capture_output=True, check=True).stdout
    return [p for p in out.decode().split("\0") if p]


def rename_repo(repo, check):
    """Renames (or, with check, only finds) what is left in repo. Returns one description per changed or left line,
    then (check only) one per tracked path with an old name in it."""
    found = []
    for rel in tracked(repo):
        if rel.startswith(SKIP):
            continue
        path = repo + "/" + rel
        with open(path, "rb") as f:
            data = f.read()
        if b"\0" in data[:8192]:
            continue
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError:
            continue
        lines = text.splitlines(keepends=True)
        changed = False
        for i, line in enumerate(lines):
            if KEEP in line:
                continue
            if check:
                if LEFT.search(line):
                    found.append("%s:%d: %s" % (rel, i + 1, line.strip()))
            else:
                new = RENAME.sub(lambda m: NEW[m.group(0)], line)
                if new != line:
                    lines[i] = new
                    changed = True
                    found.append("%s:%d" % (rel, i + 1))
        if changed:
            with open(path, "w", encoding="utf-8", newline="") as f:
                f.write("".join(lines))
    if check:
        found += ["%s (path)" % rel for rel in tracked(repo) if not rel.startswith(SKIP) and LEFT.search(rel)]
    return found


def main(argv):
    check = "--check" in argv
    args = [a for a in argv if a != "--check"]
    if len(args) != 1:
        sys.exit("usage: werk_rename.py [--check] <repo>")
    found = rename_repo(args[0], check)
    sys.stdout.reconfigure(errors="replace")   # docs have ≤ and ✅; a Windows console's cp1252 has neither
    if check:
        for line in found:
            print(line)
        print("%d left" % len(found))
        return 1 if found else 0
    print("%d lines in %d files" % (len(found), len({f.rsplit(":", 1)[0] for f in found})))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
