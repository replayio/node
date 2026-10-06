#!/bin/bash
# The RBE toolchain image (see Dockerfile next to this script).
#   toolchain-image.sh ref      print public.ecr.aws/z3x5l6b1/node-rbe-toolchain:<tag>
#   toolchain-image.sh ensure   build + push it unless that tag already exists
#                               (logs in with the agent's AWS identity)
#   toolchain-image.sh pull     pull it and print its digest ref (repo@sha256:...)
# The tag is a hash of the Dockerfile, so a tag is built once and then frozen:
# apt packages float, but every build of a given Dockerfile uses the same image.
# ECR Public, so EngFlow's workers pull it anonymously (like Chromium's gcr
# worker image). Only toolchain bits go in it; see the Dockerfile.
set -euo pipefail
dir=$(dirname "$(readlink -f "$0")")
repo=public.ecr.aws/z3x5l6b1/node-rbe-toolchain
tag=$(sha256sum "$dir/Dockerfile" | cut -c1-16)
ref=$repo:$tag

login() {
  # AWS CLI from a container: the agent host may not have one. ECR Public's
  # API only lives in us-east-1.
  docker run --rm public.ecr.aws/aws-cli/aws-cli:2.15.0 ecr-public get-login-password --region us-east-1 \
    | docker login --username AWS --password-stdin public.ecr.aws >/dev/null
}

case ${1:-} in
  ref) echo "$ref" ;;
  ensure)
    if docker manifest inspect "$ref" >/dev/null 2>&1; then
      echo "toolchain-image: $ref already published"
      exit 0
    fi
    # EngFlow workers and our agents are x86-64, also when pushing from a Mac.
    docker build --platform linux/amd64 --build-arg "TOOLCHAIN_TAG=$tag" -t "$ref" "$dir"
    login
    docker push "$ref"
    ;;
  pull)
    docker pull -q "$ref" >/dev/null
    docker image inspect --format '{{index .RepoDigests 0}}' "$ref"
    ;;
  *) echo "usage: $0 ref|ensure|pull" >&2; exit 2 ;;
esac
