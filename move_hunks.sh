#!/usr/bin/env bash
# move_hunks.sh <branch> <first> <last> <patch>
#
# Adds <patch> to the trees of commits <first>..<last> (inclusive), leaving the
# trees of every later commit on <branch> unchanged. That moves the change from
# wherever it is applied later into <first>: the later commit's diff shrinks
# because its parent already has it, and the branch tip's tree is untouched.
#
# Verifies the tip tree is unchanged and refuses to leave the branch moved
# otherwise.
set -euo pipefail
branch=$1; first=$2; last=$3; patch=$4

first=$(git rev-parse "$first"); last=$(git rev-parse "$last")
old_tip=$(git rev-parse "$branch"); old_tree=$(git rev-parse "$branch^{tree}")
parent=$(git rev-parse "$first^")

new=$parent
in_range=0
for c in $(git rev-list --reverse "$parent..$branch"); do
  git read-tree "$c"
  [ "$c" = "$first" ] && in_range=1
  if [ "$in_range" = 1 ]; then
    git apply --cached "$patch"
  fi
  [ "$c" = "$last" ] && in_range=0
  tree=$(git write-tree)
  new=$(GIT_AUTHOR_NAME=$(git log -1 --format=%an "$c") \
        GIT_AUTHOR_EMAIL=$(git log -1 --format=%ae "$c") \
        GIT_AUTHOR_DATE=$(git log -1 --format=%aI "$c") \
        GIT_COMMITTER_NAME=$(git log -1 --format=%cn "$c") \
        GIT_COMMITTER_EMAIL=$(git log -1 --format=%ce "$c") \
        GIT_COMMITTER_DATE=$(git log -1 --format=%cI "$c") \
        git commit-tree "$tree" -p "$new" -F <(git log -1 --format=%B "$c"))
done

new_tree=$(git rev-parse "$new^{tree}")
if [ "$new_tree" != "$old_tree" ]; then
  echo "FAIL: tip tree changed ($old_tree -> $new_tree); branch left at $old_tip" >&2
  exit 1
fi
git read-tree "$new"
git update-ref "refs/heads/$(git rev-parse --abbrev-ref "$branch")" "$new" "$old_tip"
git checkout -q -f "$(git rev-parse --abbrev-ref "$branch")"
echo "moved; $branch $old_tip -> $new (tree unchanged: $new_tree)"
