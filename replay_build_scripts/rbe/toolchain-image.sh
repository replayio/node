#!/bin/bash
# The RBE toolchain image (see Dockerfile next to this script).
#   toolchain-image.sh ref      print public.ecr.aws/z3x5l6b1/node-rbe-toolchain:<tag>
#   toolchain-image.sh ensure   build + push it unless that tag already exists
#                               (AWS identity with ECR Public push rights)
#   toolchain-image.sh pull     pull it and print its digest ref (repo@sha256:...)
#   toolchain-image.sh digest   print its digest ref without docker (host builds)
# The tag is a hash of the Dockerfile, and an existing tag is never pushed
# again: apt packages float, but every build of a given Dockerfile uses the same
# image. ECR Public, so EngFlow's workers pull it anonymously (like Chromium's
# gcr worker image). Only toolchain bits go in it; see the Dockerfile.
set -euo pipefail
dir=$(dirname "$(readlink -f "$0")")
repo=public.ecr.aws/z3x5l6b1/node-rbe-toolchain
tag=$(sha256sum "$dir/Dockerfile" | cut -c1-16)
ref=$repo:$tag

# Registry access goes through a throwaway docker config: anonymous for checks
# and pulls, and a push login never outlives the push. Expired public.ecr.aws
# credentials left in ~/.docker would make even anonymous pulls fail.
config=$(mktemp -d)
trap 'rm -rf "$config"' EXIT
registry() { DOCKER_CONFIG=$config docker "$@"; }

# Exit status 0: tag exists, 1: tag doesn't exist. Anything else is an error,
# never a reason to (re)build.
published() {
  local out
  if out=$(registry manifest inspect "$ref" 2>&1); then
    return 0
  fi
  if grep -qiE "manifest unknown|no such manifest|not found" <<<"$out"; then
    return 1
  fi
  echo "toolchain-image: can't check $ref: $out" >&2
  exit 1
}

login() {
  # ECR Public's API only lives in us-east-1. Without an AWS CLI on the host,
  # use it from a container (instance role via IMDS).
  if command -v aws >/dev/null; then
    aws ecr-public get-login-password --region us-east-1
  else
    docker run --rm public.ecr.aws/aws-cli/aws-cli:2.15.0 ecr-public get-login-password --region us-east-1
  fi | registry login --username AWS --password-stdin public.ecr.aws >/dev/null
}

case ${1:-} in
  ref) echo "$ref" ;;
  ensure)
    if published; then
      echo "toolchain-image: $ref already published"
      exit 0
    fi
    # EngFlow workers and our agents are x86-64, also when pushing from a Mac.
    docker build --platform linux/amd64 --build-arg "TOOLCHAIN_TAG=$tag" -t "$ref" "$dir"
    login
    # Another pipeline may have published it while this one was building.
    if published; then
      echo "toolchain-image: $ref was published meanwhile; not overwriting it"
      exit 0
    fi
    registry push "$ref"
    ;;
  digest)
    # Anonymous registry API, so hosts without docker can resolve it too.
    name=${repo#public.ecr.aws/}
    token=$(curl -sf "https://public.ecr.aws/token/?scope=repository:$name:pull" | sed -n 's/.*"token":"\([^"]*\)".*/\1/p')
    digest=$(curl -sfI -H "Authorization: Bearer $token" \
      -H "Accept: application/vnd.oci.image.index.v1+json, application/vnd.docker.distribution.manifest.list.v2+json, application/vnd.oci.image.manifest.v1+json, application/vnd.docker.distribution.manifest.v2+json" \
      "https://public.ecr.aws/v2/$name/manifests/$tag" | tr -d '\r' | sed -n 's/^docker-content-digest: *//Ip')
    if [ -z "$digest" ]; then
      echo "toolchain-image: $ref is not published" >&2
      exit 1
    fi
    echo "$repo@$digest"
    ;;
  pull)
    registry pull -q "$ref" >/dev/null
    # The same image can carry digests from other repositories on this host.
    docker image inspect --format '{{range .RepoDigests}}{{println .}}{{end}}' "$ref" | grep -m1 "^$repo@"
    ;;
  *) echo "usage: $0 ref|ensure|pull|digest" >&2; exit 2 ;;
esac
