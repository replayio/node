# usage: build_stack.sh <scratchdir>
# Rebuilds a curated stack: cherry-picks each commit of the previous stack in
# order, folds its group commit (G:<n>) if any, inserts the new topic commits
# (G:N-*) at their declared positions, drops commits upstream fully reverted,
# and merges commits whose code upstream merged. Expects <scratchdir>/wt to be
# a clean worktree with the previous stack and the group commits available.
set -e
S="$1"; cd $S/wt
declare -A G
while read -r sha sub; do G[${sub#G:}]=$sha; done < <(git log --reverse --format='%h %s' rewrite/v7..v8-groups)
V7=($(git rev-list --reverse 85e3624473..rewrite/v7))
# insertion points: after v7 position N, these groups (in order)
declare -A INS
INS[4]="N-RBE"; INS[17]="N-WEAKREF"; INS[26]="N-BRKPT"; INS[34]="N-GC N-ACTIVITY"
INS[39]="N-THROW-PROC"; INS[40]="N-INTERRUPTS N-NEARHEAP N-FUTEX N-FINAL"; INS[42]="N-THROW-FS"
# v7 commits to merge into the preceding commit (upstream merged the code)
declare -A MERGE_INTO; MERGE_INTO[35]=34
git reset -q --hard; git clean -qfd; git switch -q --detach 85e3624473; git branch -f v8-pre HEAD; git switch -q v8-pre
for i in $(seq 1 43); do
  c=${V7[$((i-1))]}
  if [ -n "${MERGE_INTO[$i]:-}" ]; then
    git cherry-pick -n $c >/dev/null || { echo "MERGE PICK FAIL $i"; exit 1; }
    [ -n "${G[$i]:-}" ] && { git cherry-pick -n ${G[$i]} >/dev/null || { echo "MERGE FOLD FAIL $i"; exit 1; }; }
    git commit -q --amend --no-edit
    echo "MERGED $i into previous commit"
    continue
  fi
  if ! git cherry-pick $c >/dev/null 2>&1; then
    if git diff --cached --quiet HEAD 2>/dev/null && [ -z "$(git status --porcelain)" ]; then
      git cherry-pick --skip >/dev/null 2>&1 || git reset -q --hard
      echo "DROPPED (already present) $i $(git log -1 --format=%s $c)"; continue
    fi
    echo "PICK FAIL $i $(git log -1 --format=%s $c)"; exit 1
  fi
  if [ -n "${G[$i]:-}" ]; then
    git cherry-pick -n ${G[$i]} >/dev/null || { echo "FOLD FAIL $i: $(git diff --name-only --diff-filter=U | tr "\n" " ")"; exit 1; }
    if git diff --cached --quiet HEAD~1 2>/dev/null; then
      # the whole commit is obsolete: upstream reverted its patch
      echo "DROPPED (fully reverted) $i $(git log -1 --format=%s HEAD)"
      git reset -q --hard HEAD~1
    else
      git commit -q --amend --no-edit
    fi
  fi
  for g in ${INS[$i]:-}; do
    git cherry-pick -n ${G[$g]} >/dev/null || { echo "INSERT FAIL $g after $i: $(git diff --name-only --diff-filter=U | tr "\n" " ")"; exit 1; }
    git commit -q -m "$g (placeholder)"
  done
done
echo "commits: $(git rev-list --count 85e3624473..HEAD)"
echo "diff vs master: [$(git diff --shortstat origin/master HEAD)]"
