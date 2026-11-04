#!/usr/bin/env python3
"""Rewrite a stack's commit messages from a messages file.

usage: apply_messages.py <BASE> <STACK_REF> <messages> <NEW_BRANCH> [--trailer]

messages holds blocks:
    === <sha-or-prefix>
    <message text>
Commits not named keep their message. Trees, authors and dates are preserved.
With --trailer, a Claude Co-Authored-By line is appended to every message that
was replaced (attribution tooling later adds the human trailers).
"""
import os, re, subprocess, sys

BASE, STACK, MSGS, BRANCH = sys.argv[1:5]
TRAILER = "--trailer" in sys.argv[5:]
CLAUDE = "Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"

def git(*a, env=None, inp=None):
    r = subprocess.run(["git", *a], capture_output=True, text=True, env=env, input=inp)
    if r.returncode:
        sys.exit(r.stderr)
    return r.stdout.strip()

blocks, key = {}, None
for line in open(MSGS).read().split("\n"):
    m = re.match(r"^===\s+([0-9a-f]{7,40})\s*$", line)
    if m:
        key = git("rev-parse", m.group(1))
        blocks[key] = []
    elif key:
        blocks[key].append(line)
msgs = {k: "\n".join(v).strip("\n") + "\n" for k, v in blocks.items()}

parent, used = git("rev-parse", BASE), set()
for c in git("rev-list", "--reverse", f"{BASE}..{STACK}").split():
    an, ae, ad, old = git("log", "-1", "--format=%an%x00%ae%x00%aI%x00%B", c).split("\x00", 3)
    new = msgs.get(c)
    if new is not None:
        used.add(c)
        if TRAILER and "anthropic.com" not in new:
            new = new.rstrip("\n") + "\n\n" + CLAUDE + "\n"
    else:
        new = old if old.endswith("\n") else old + "\n"
    env = dict(os.environ, GIT_AUTHOR_NAME=an, GIT_AUTHOR_EMAIL=ae, GIT_AUTHOR_DATE=ad)
    parent = git("commit-tree", c + "^{tree}", "-p", parent, "-F", "-", env=env, inp=new)
git("branch", "-f", BRANCH, parent)
missing = set(msgs) - used
print(f"built {BRANCH} {parent[:11]}: {len(used)} messages replaced, {len(missing)} unused")
for m in missing:
    print(f"  unused block: {m[:11]} {git('log','-1','--format=%s',m)}")
