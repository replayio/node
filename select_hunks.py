#!/usr/bin/env python3
"""Select hunks from a unified diff into a smaller applicable patch.

usage: select_hunks.py <diff> <out.patch> SPEC...
       SPEC = path            -> all hunks of that file
              path:1,3,7      -> only those hunks (1-based, in file order)

The output is applied with `git apply --index`. Used to split one big
"delta" diff (v7..master) into one patch per destination commit.
"""
import sys

def parse(path):
    files = {}
    cur = None
    for line in open(path, errors="replace").read().splitlines(keepends=True):
        if line.startswith("diff --git"):
            cur = line.split(" b/")[-1].rstrip("\n")
            files[cur] = {"hdr": [line], "hunks": []}
        elif cur is None:
            continue
        elif line.startswith("@@"):
            files[cur]["hunks"].append([line])
        elif files[cur]["hunks"]:
            files[cur]["hunks"][-1].append(line)
        else:
            files[cur]["hdr"].append(line)
    return files

def main():
    diff, out, specs = sys.argv[1], sys.argv[2], sys.argv[3:]
    files = parse(diff)
    chunks = []
    for spec in specs:
        path, _, idx = spec.partition(":")
        if path not in files:
            sys.exit(f"select_hunks: no such file in diff: {path}")
        f = files[path]
        wanted = [int(i) for i in idx.split(",")] if idx else range(1, len(f["hunks"]) + 1)
        for i in wanted:
            if not 1 <= i <= len(f["hunks"]):
                sys.exit(f"select_hunks: {path} has {len(f['hunks'])} hunks, asked for {i}")
        chunks.append("".join(f["hdr"]) + "".join("".join(f["hunks"][i - 1]) for i in wanted))
    open(out, "w").write("".join(chunks))
    print(f"{out}: {sum(c.count(chr(10) + '@@ ') + c.startswith('@@ ') for c in chunks)} hunks from {len(specs)} spec(s)")

main()
