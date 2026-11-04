#!/usr/bin/env python3
"""Rewrite a stack's commit messages with Co-authored-by trailers.

usage: apply_trailers.py <BASE> <STACK_REF> <attr.json> <NEW_BRANCH>

Keeps each commit's tree, author and date; replaces the trailer block with
one Co-authored-by per original author of any line in that commit (most
lines first), dropping the commit's own author. An existing Claude
trailer is preserved last. Existing non-bot trailers are kept even if
blame no longer credits them.
"""
import json, os, re, subprocess, sys

BASE, STACK, ATTR, BRANCH = sys.argv[1:5]
CLAUDE = "Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
CANON = {"domiii@users.noreply.github.com": ("Domi", "Domiii@users.noreply.github.com")}
d = json.load(open(ATTR))
P = d["people"]

def git(*a, env=None, inp=None):
    r = subprocess.run(["git", *a], capture_output=True, text=True, env=env, input=inp)
    if r.returncode:
        sys.exit(r.stderr)
    return r.stdout.strip()

parent = git("rev-parse", BASE)
for c in git("rev-list", "--reverse", f"{BASE}..{STACK}").split():
    an, ae, ad, msg = git("log", "-1", "--format=%an%x00%ae%x00%aI%x00%B", c).split("\x00", 3)
    body, tr = [], {}
    for l in msg.rstrip("\n").split("\n"):
        m = re.match(r"(?i)co-authored-by:\s*(.*?)\s*<(.*)>", l)
        if m:
            tr[m.group(2).lower()] = m.group(1)
        else:
            body.append(l)
    while body and body[-1] == "":
        body.pop()
    has_claude = any("anthropic.com" in e for e in tr)
    cnt = {}
    for k in ("add", "del"):
        for e, n in d[k].get(c, {}).items():
            cnt[e] = cnt.get(e, 0) + n
    for e in tr:
        if "anthropic.com" not in e:
            cnt.setdefault(e, 0)
    cnt.pop(ae.lower(), None)
    co = []
    for e in sorted(cnt, key=lambda e: -cnt[e]):
        name, mail = CANON.get(e, (P.get(e) or tr.get(e) or e, e))
        co.append(f"Co-authored-by: {name} <{mail}>")
    new = "\n".join(body) + "\n"
    if co or has_claude:
        new += "\n" + "\n".join(co + ([CLAUDE] if has_claude else [])) + "\n"
    env = dict(os.environ, GIT_AUTHOR_NAME=an, GIT_AUTHOR_EMAIL=ae, GIT_AUTHOR_DATE=ad)
    parent = git("commit-tree", c + "^{tree}", "-p", parent, "-F", "-", env=env, inp=new)
git("branch", "-f", BRANCH, parent)
print(f"built {BRANCH} {parent[:10]}")
