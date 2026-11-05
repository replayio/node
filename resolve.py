#!/usr/bin/env python3
"""Resolve one conflict region in a diff3-style conflicted file.

  resolve.py <file> <n> ours|theirs|base          -> take that side verbatim
  resolve.py <file> <n> body                      -> replace with stdin
  resolve.py <file> list                          -> print the conflict regions

<n> is 1-based over the conflict regions remaining in the file.
"""
import re, sys

PAT = re.compile(
    r"^<<<<<<< [^\n]*\n(?P<ours>.*?)"
    r"^\|\|\|\|\|\|\| [^\n]*\n(?P<base>.*?)"
    r"^=======\n(?P<theirs>.*?)"
    r"^>>>>>>> [^\n]*\n",
    re.S | re.M,
)

def main():
    path = sys.argv[1]
    s = open(path).read()
    hits = list(PAT.finditer(s))
    if sys.argv[2] == "list":
        for i, m in enumerate(hits, 1):
            print(f"--- #{i} ours:");   sys.stdout.write(m.group("ours"))
            print(f"--- #{i} theirs:"); sys.stdout.write(m.group("theirs"))
        return
    n = int(sys.argv[2])
    if not 1 <= n <= len(hits):
        sys.exit(f"{path}: conflict #{n} of {len(hits)} not found")
    m = hits[n - 1]
    mode = sys.argv[3]
    repl = sys.stdin.read() if mode == "body" else m.group(mode)
    open(path, "w").write(s[: m.start()] + repl + s[m.end():])
    print(f"{path}: resolved #{n} ({mode}), {len(hits)-1} left")

main()
