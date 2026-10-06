#!/bin/bash
# The RBE toolchain image (see Dockerfile next to this script).
#   toolchain-image.sh ref      print ghcr.io/replayio/node-rbe-toolchain:<tag>
#   toolchain-image.sh ensure   build + push it unless that tag already exists
#                               (needs GHCR_USER/GHCR_TOKEN with write:packages)
#   toolchain-image.sh pull     pull it and print its digest ref (repo@sha256:...)
# The tag is a hash of the Dockerfile, so a tag is built once and then frozen:
# apt packages float, but every build of a given Dockerfile uses the same image.
set -euo pipefail
dir=$(dirname "$(readlink -f "$0")")
repo=ghcr.io/replayio/node-rbe-toolchain
tag=$(sha256sum "$dir/Dockerfile" | cut -c1-16)
ref=$repo:$tag

case ${1:-} in
  ref) echo "$ref" ;;
  ensure)
    if docker manifest inspect "$ref" >/dev/null 2>&1; then
      echo "toolchain-image: $ref already published"
      exit 0
    fi
    docker build --build-arg "TOOLCHAIN_TAG=$tag" -t "$ref" "$dir"
    echo "$GHCR_TOKEN" | docker login ghcr.io -u "$GHCR_USER" --password-stdin
    docker push "$ref"
    ;;
  pull)
    docker pull -q "$ref" >/dev/null
    docker image inspect --format '{{index .RepoDigests 0}}' "$ref"
    ;;
  *) echo "usage: $0 ref|ensure|pull" >&2; exit 2 ;;
esac
