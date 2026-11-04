#!/usr/bin/env python3
"""Compute per-commit original-author attribution for a rewritten stack.

usage: attribute.py <BASE> <STACK_REF> <HISTORY_REF> <out.json>

BASE        upstream base both histories sit on (e.g. the v16.14.3 commit)
STACK_REF   the rewritten stack (its final tree must equal HISTORY_REF's)
HISTORY_REF the original history (e.g. origin/master)

Blames every file on both refs; each line links the stack commit that adds
it to the original commit that wrote it. Deleted upstream lines are matched
by (file, content) against the original commit that removed them. Human
Co-authored-by trailers on original commits count too; bots are skipped.
"""
import collections, json, re, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

BASE, STACK, HIST, OUT = sys.argv[1:5]
BOTS = ("cursoragent@", "noreply@anthropic.com")

def git(*a):
    return subprocess.run(["git", *a], capture_output=True, text=True, errors="replace").stdout

people, cinfo = {}, {}
for c in git("rev-list", f"{BASE}..{HIST}").split():
    an, ae, tr = git("log", "-1",
                     "--format=%an%x00%ae%x00%(trailers:key=Co-authored-by,valueonly,separator=%x01)",
                     c).rstrip("\n").split("\x00")
    s = {ae.lower()}
    people.setdefault(ae.lower(), an)
    for t in filter(None, tr.split("\x01")):
        m = re.match(r"\s*(.*?)\s*<(.*)>", t)
        if m and not any(b in m.group(2) for b in BOTS):
            s.add(m.group(2).lower())
            people.setdefault(m.group(2).lower(), m.group(1))
    cinfo[c] = s

def blame(rev, f):
    out, res, cur = git("blame", "--porcelain", f"{BASE}..{rev}", "--", f), [], None
    for l in out.split("\n"):
        m = re.match(r"^([0-9a-f]{40}) \d+ (\d+)", l)
        if m:
            cur = m.group(1)
        elif l.startswith("\t"):
            res.append(cur)
        elif l.startswith("boundary"):
            cur = "BOUNDARY"
    return res

agg = collections.defaultdict(collections.Counter)
src = collections.defaultdict(set)
files = git("diff", "--name-only", "--diff-filter=AM", BASE, HIST).split()
with ThreadPoolExecutor(8) as ex:
    for f, a, b in ex.map(lambda f: (f, blame(STACK, f), blame(HIST, f)), files):
        if len(a) != len(b):
            print(f"warn: blame length mismatch for {f}", file=sys.stderr)
            continue
        for x, y in zip(a, b):
            if y in cinfo and x not in (None, "BOUNDARY"):
                for e in cinfo[y]:
                    agg[x][e] += 1
                src[x].add(y)

def minus(c):
    d, f = collections.defaultdict(list), None
    for l in git("show", "--format=", "--no-renames", "-U0", c).split("\n"):
        if l.startswith("--- a/"):
            f = l[6:]
        elif l.startswith("--- /dev/null"):
            f = None
        elif l.startswith("-") and not l.startswith("---") and f:
            s = l[1:].strip()
            if len(s) > 3 and re.search(r"[A-Za-z0-9]", s):
                d[f].append(s)
    return d

idx = collections.defaultdict(list)
for c in reversed(git("rev-list", "--no-merges", f"{BASE}..{HIST}").split()):
    for f, ls in minus(c).items():
        for s in set(ls):
            idx[(f, s)].append(c)

delagg = collections.defaultdict(collections.Counter)
for c in git("rev-list", f"{BASE}..{STACK}").split():
    for f, ls in minus(c).items():
        for s in ls:
            if idx.get((f, s)):
                y = idx[(f, s)][0]
                for e in cinfo[y]:
                    delagg[c][e] += 1
                src[c].add(y)

json.dump({"people": people,
           "add": {k: dict(v) for k, v in agg.items()},
           "del": {k: dict(v) for k, v in delagg.items()},
           "src": {k: sorted(v) for k, v in src.items()}},
          open(OUT, "w"), indent=1)
print(f"{OUT}: {len(agg)} commits attributed, {len(people)} people")
