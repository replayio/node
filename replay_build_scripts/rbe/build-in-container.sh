#!/bin/bash
# Build this node checkout inside the toolchain image: for hosts whose
# toolchain differs from the RBE workers' (build-rbe.py refuses those), or to
# build exactly like CI does. Same as `node build.js` otherwise, with the same
# environment: ENGFLOW_CERT_FILE / ENGFLOW_KEY_FILE (or RBE_tls_client_auth_*),
# and optionally DRIVER_REVISION, REPLAY_LOCAL_DRIVER_DIR,
# REPLAY_RBE_REQUIRE_REMOTE.
#
#   replay_build_scripts/rbe/build-in-container.sh
#
# The checkout (and a worktree's shared git directory) is mounted at its own
# path, so host and container builds of a checkout share out/ without
# rebuilding. Files the container writes are handed back to the caller.
set -euo pipefail
dir=$(dirname "$(readlink -f "$0")")
root=$(dirname "$(dirname "$dir")")
image=$("$dir/toolchain-image.sh" pull)

cert=${ENGFLOW_CERT_FILE:-${RBE_tls_client_auth_cert:-}}
key=${ENGFLOW_KEY_FILE:-${RBE_tls_client_auth_key:-}}
if [ ! -r "$cert" ] || [ ! -r "$key" ]; then
  echo "build-in-container: set ENGFLOW_CERT_FILE / ENGFLOW_KEY_FILE to the EngFlow client cert/key files" >&2
  exit 1
fi

args=(
  -v "$root:$root" -w "$root"
  -v "$(readlink -f "$cert"):/engflow/engflow.crt:ro"
  -v "$(readlink -f "$key"):/engflow/engflow.key:ro"
  -e ENGFLOW_CERT_FILE=/engflow/engflow.crt
  -e ENGFLOW_KEY_FILE=/engflow/engflow.key
  -e "REPLAY_RBE_CONTAINER_IMAGE=$image"
)
for var in RBE_service RBE_service_no_auth RBE_use_application_default_credentials \
           DRIVER_REVISION REPLAY_RBE_REQUIRE_REMOTE; do
  if [ -n "${!var:-}" ]; then
    args+=(-e "$var")
  fi
done
# A worktree's .git points into the main repository's git directory.
git_common=$(git -C "$root" rev-parse --path-format=absolute --git-common-dir)
case "$git_common" in
  "$root"/*) ;;
  *) args+=(-v "$git_common:$git_common:ro") ;;
esac
if [ -n "${REPLAY_LOCAL_DRIVER_DIR:-}" ]; then
  driver=$(cd "$REPLAY_LOCAL_DRIVER_DIR" && pwd)
  args+=(-v "$driver:$driver:ro" -e "REPLAY_LOCAL_DRIVER_DIR=$driver")
fi

# The container runs as root; hand the files it writes back to the caller.
# Only root's files: chown bumps ctime, which makes git rescan the whole tree.
trap 'docker run --rm -v "$root:$root" --entrypoint chown "$image" -R --from=0:0 "$(id -u):$(id -g)" "$root"' EXIT
docker run --rm "${args[@]}" "$image" node build.js
