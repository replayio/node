#!/usr/bin/env python3
"""Assign every hunk of a delta diff to a destination commit, mechanically.

usage: assign_hunks.py <OLD> <NEW> <prmap> <outdir> <STACK_REF> [overrides]

overrides  optional lines "path:hunk=DEST" or "path=DEST", applied last

OLD/NEW   the range whose delta is being folded (e.g. rewrite/v7..origin/master)
prmap     lines "PR=DEST", plus "SHA=DEST" for commits with no PR number, and
          "<PR>=OWNER" meaning "the destination that owns the file"
outdir    gets <DEST>.spec files (select_hunks.py specs) + report.txt

Each added line is blamed to the commit that wrote it in NEW; that commit's PR
gives the destination. Deletion-only hunks are matched by (file, content)
against the commit that removed the line. A hunk's destination is the one
holding the most of its changed lines; hunks whose lines disagree are listed in
the report as MIXED so the placement can be reviewed (content is unaffected).
"""
import collections, json, re, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

OLD, NEW, PRMAP, OUT, STACK = sys.argv[1:6]
OVERRIDES = sys.argv[6] if len(sys.argv) > 6 else None
# OLD must be the ancestor of NEW in the real history (blame range);
# STACK is the rewritten stack whose commits own files (same tree as OLD).

def git(*a):
    return subprocess.run(["git", *a], capture_output=True, text=True, errors="replace").stdout

raw = dict(l.split("=", 1) for l in open(PRMAP).read().split() if "=" in l)
# commit -> dest
subj = {}
for c in git("rev-list", f"{OLD}..{NEW}").split():
    s = git("log", "-1", "--format=%s", c).strip()
    m = re.search(r"\(#(\d+)\)", s)
    key = m.group(1) if m else git("rev-parse", "--short=11", c).strip()
    subj[c] = raw.get(key, "?" + key)

owners = {}
def owner_of(f):
    if f not in owners:
        o = [s for s in git("log", "--format=%s", f"{OWNER_BASE}..{OWNER_REF}", "--", f).strip().split("\n") if s]
        owners[f] = o
    return owners[f]
OWNER_BASE, OWNER_REF = "85e3624473", STACK

def dest_for(commit, f):
    d = subj.get(commit, "?unknown")
    if d == "OWNER":
        o = owner_of(f)
        return OWNER_NUM.get(o[0], "OWNER-AMBIG") if len(o) == 1 else "OWNER-AMBIG"
    return d

# v7 subject -> stack position, so OWNER can resolve to a number
OWNER_NUM = {}
for i, s in enumerate(git("log", "--reverse", "--format=%s", f"{OWNER_BASE}..{OWNER_REF}").strip().split("\n"), 1):
    OWNER_NUM[s] = str(i)

def blame_map(f):
    out, res, cur = git("blame", "--porcelain", f"{OLD}..{NEW}", "--", f), {}, None
    n = 0
    for l in out.split("\n"):
        m = re.match(r"^([0-9a-f]{40}) \d+ (\d+)", l)
        if m:
            cur, n = m.group(1), int(m.group(2))
        elif l.startswith("\t"):
            res[n] = cur
    return res

# deletion index: (file, stripped content) -> first commit removing it
def minus(c):
    d, f = collections.defaultdict(list), None
    for l in git("show", "--format=", "--no-renames", "-U0", c).split("\n"):
        if l.startswith("--- a/"):
            f = l[6:]
        elif l.startswith("--- /dev/null"):
            f = None
        elif l.startswith("-") and not l.startswith("---") and f:
            s = l[1:].strip()
            if s:
                d[f].append(s)
    return d

delidx = collections.defaultdict(list)
for c in reversed(git("rev-list", "--no-merges", f"{OLD}..{NEW}").split()):
    for f, ls in minus(c).items():
        for s in set(ls):
            delidx[(f, s)].append(c)

# parse delta
delta = git("diff", "--full-index", OLD, NEW)
files, cur = {}, None
for line in delta.splitlines(keepends=True):
    if line.startswith("diff --git"):
        cur = line.split(" b/")[-1].rstrip("\n")
        files[cur] = []
    elif cur and line.startswith("@@"):
        files[cur].append([line])
    elif cur and files[cur]:
        files[cur][-1].append(line)

blames = {}
with ThreadPoolExecutor(8) as ex:
    for f, b in ex.map(lambda f: (f, blame_map(f)), list(files)):
        blames[f] = b

over = {}
if OVERRIDES:
    for l in open(OVERRIDES):
        l = l.split("#")[0].strip()
        if l:
            k, _, v = l.rpartition("=")
            over[k.strip()] = v.strip()

specs, report, mixed, unknown = collections.defaultdict(list), [], 0, 0
for f, hunks in files.items():
    for i, h in enumerate(hunks, 1):
        m = re.match(r"@@ -\d+(?:,\d+)? \+(\d+)", h[0])
        newline = int(m.group(1)) if m else 0
        votes = collections.Counter()
        n = newline
        for l in h[1:]:
            if l.startswith("+"):
                c = blames[f].get(n)
                if c:
                    votes[dest_for(c, f)] += 1
                n += 1
            elif l.startswith("-"):
                s = l[1:].strip()
                cs = delidx.get((f, s))
                if cs:
                    votes[dest_for(cs[0], f)] += 1
            elif not l.startswith("\\"):
                n += 1
        if not votes:
            ov = over.get(f"{f}:{i}") or over.get(f)
            if ov:
                specs[ov].append(f"{f}:{i}")
                report.append(f"OVERRIDE(novote) {f}:{i} -> {ov}")
                continue
            report.append(f"NOVOTE {f}:{i}")
            unknown += 1
            continue
        best, cnt = votes.most_common(1)[0]
        if len(votes) > 1:
            mixed += 1
            report.append(f"MIXED  {f}:{i} -> {best} ({dict(votes)})")
        # A hunk cannot fold into a commit earlier than the one that owns its
        # file: its context may not exist yet. Bump to the last owner, which is
        # the area commit for that code anyway.
        if best.isdigit():
            op = [OWNER_NUM[s] for s in owner_of(f) if s in OWNER_NUM]
            if op and max(int(x) for x in op) > int(best):
                bumped = str(max(int(x) for x in op))
                report.append(f"BUMP   {f}:{i} {best} -> {bumped} (file owner)")
                best = bumped
        ov = over.get(f"{f}:{i}") or over.get(f)
        if ov:
            best = ov
        if best.startswith("?") or best == "OWNER-AMBIG":
            report.append(f"UNMAPPED {f}:{i} -> {best}")
            unknown += 1
        specs[best].append(f"{f}:{i}")

import os
os.makedirs(OUT, exist_ok=True)
for d, items in specs.items():
    byfile = collections.defaultdict(list)
    for it in items:
        f, _, i = it.rpartition(":")
        byfile[f].append(int(i))
    with open(f"{OUT}/{d}.spec", "w") as fh:
        for f, idxs in byfile.items():
            fh.write(f"{f}:{','.join(str(i) for i in sorted(idxs))}\n")
open(f"{OUT}/report.txt", "w").write("\n".join(report) + "\n")
tot = sum(len(v) for v in files.values())
print(f"hunks: {tot}  destinations: {len(specs)}  mixed: {mixed}  unmapped/novote: {unknown}")
print("per-destination hunk counts:")
for d, items in sorted(specs.items(), key=lambda x: -len(x[1])):
    print(f"  {d:16s} {len(items)}")
