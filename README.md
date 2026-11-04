# rewrite-helpers

Tooling for maintaining the Replay fork's **clean-slate history**: one
topic-per-commit stack whose final content is byte-identical to `master`,
kept rebasable onto newer upstream Node.

This branch is an **orphan** (no parent, not related to `master` or to any
`rewrite/*` branch) purely so the tooling survives worktree recreation and
can never interfere with a rebase. Do not merge it anywhere.

    git show origin/tools/rewrite-helpers:select_hunks.py > /tmp/select_hunks.py

## Scripts

| script | what it does |
|---|---|
| `select_hunks.py` | split one big delta diff into a patch per destination commit (`path` or `path:1,3,7`) |
| `attribute.py` | blame both histories, map each stack commit to the original authors of its lines |
| `apply_trailers.py` | rewrite the stack adding `Co-authored-by` trailers from that attribution |
| `verify_stack.sh` | tree identity, empty commits, conflict markers, authors, range-diff vs previous iteration |

## The rules (established with the user)

Curating the stack
- Rebase the curated stack, never master's raw history.
- New master commits are never appended as-is: fold each hunk into the commit that owns that code.
- Only self-contained features earn a new commit, placed right after the commit it depends on.
- Author stays whoever wrote the original commit; a new commit is authored by whoever wrote that work.

Correctness bar
- Result must be byte-identical to master: same tree hash, empty diff.
- Build the folds as separate "group" commits first, confirm they sum to master, then rebuild the stack.
- Account for every hunk in the delta.
- Per-commit: nothing used before the commit that introduces it, no empty commits, no stale/renamed APIs.
- `range-diff` against the previous iteration to prove only intended commits changed.

Messages
- Extend the description with the new behaviour; add `Includes later changes from #NNN.`
- Every commit carries a rebase note stating the invariant it enforces.
- `Co-authored-by` for every original author of any hunk (blame-derived); tools excluded.

Rebasing onto a new base
- Resolve independently; don't consult earlier rebase attempts.
- Keep upstream's new code, re-apply our intent on top.
- Log every conflict and every change not in the source stack (scratch file, never committed).
- Verify commits in == out, no conflict markers, every per-file patch-size change explained, then build.
- Branch naming: `rewrite/vN`, `rewrite/vN-on-<base>`.

Reviewing a rebase
- Only two questions: are our hunks still semantically right in the new context, and did upstream add
  new code paths our instrumentation should cover?
- Pre-existing bugs are out of scope.

## History so far

| branch | content |
|---|---|
| `rewrite/v1`..`v4` | iterations on upstream v16.14.3, content == master at PR #118 |
| `rewrite/v5`,`v6` | content == master at #136; v6 adds full blame-based attribution |
| `rewrite/v7` | 43 commits, content == master at #147 (4e87717a2c) |
| `rewrite/v7-on-16.20.3` | v7 rebased onto 76f9a7d27f (v16.20.2+1); builds, 12 conflicts resolved |
