#!/bin/bash
# usage: verify_stack.sh <BASE> <STACK_REF> <EXPECTED_CONTENT_REF> [PREV_STACK_REF]
# Checks a rewritten stack: content identity, per-commit sanity, and (with
# PREV_STACK_REF) which commits changed since the previous iteration.
set -u
BASE=$1; STACK=$2; WANT=$3; PREV=${4:-}
ok() { echo "  ok   $*"; }; bad() { echo "  FAIL $*"; FAILED=1; }
FAILED=0
echo "== content identity"
[ "$(git rev-parse "$STACK^{tree}")" = "$(git rev-parse "$WANT^{tree}")" ] \
  && ok "tree == $WANT" || bad "tree differs from $WANT: $(git diff --shortstat "$WANT" "$STACK")"
echo "== commits"
echo "  $(git rev-list --count "$BASE..$STACK") commits on $BASE"
echo "== empty commits"
e=0; for c in $(git rev-list "$BASE..$STACK"); do
  [ "$(git diff-tree --no-commit-id --name-only -r "$c" | wc -l)" -eq 0 ] && { bad "empty: $(git log -1 --format='%h %s' "$c")"; e=1; }
done; [ $e -eq 0 ] && ok "no empty commits"
echo "== conflict markers"
m=$(git grep -l -E '^(<<<<<<<|>>>>>>>) ' "$STACK" -- . 2>/dev/null | grep -v 'deps/npm/test' | wc -l)
[ "$m" -eq 0 ] && ok "none" || { git grep -l -E '^(<<<<<<<|>>>>>>>) ' "$STACK" -- . | grep -v 'deps/npm/test'; bad "$m file(s) with conflict markers"; }
echo "== author preserved on every commit"
git log --format='%an' "$BASE..$STACK" | sort -u | sed 's/^/  author: /'
if [ -n "$PREV" ]; then
  echo "== changed vs $PREV (range-diff, non-identical only)"
  git range-diff -s --no-color "$BASE..$PREV" "$BASE..$STACK" | grep -vE '^ *[0-9]+: *[0-9a-f]+ = ' | sed 's/^/  /'
fi
echo; [ $FAILED -eq 0 ] && echo "RESULT: pass" || echo "RESULT: FAIL"
exit $FAILED
