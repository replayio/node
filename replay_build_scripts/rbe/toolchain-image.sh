#!/bin/bash
# The RBE toolchain image (see Dockerfile next to this script).
#   toolchain-image.sh ref      print <ecr>/linux_node_build:<tag>
#   toolchain-image.sh ensure   build + push it unless that tag already exists
#   toolchain-image.sh pull     pull it and print its digest ref (repo@sha256:...)
# ensure/pull log in to ECR with the agent's AWS identity (instance role).
# The tag is a hash of the Dockerfile, so a tag is built once and then frozen:
# apt packages float, but every build of a given Dockerfile uses the same image.
# EngFlow's workers pull it from the same private ECR repository.
set -euo pipefail
dir=$(dirname "$(readlink -f "$0")")
registry=165324622525.dkr.ecr.us-east-2.amazonaws.com
repo=$registry/linux_node_build
tag=$(sha256sum "$dir/Dockerfile" | cut -c1-16)
ref=$repo:$tag

login() {
  # AWS CLI from a container: the agent host may not have one.
  docker run --rm public.ecr.aws/aws-cli/aws-cli:2.15.0 ecr get-login-password --region us-east-2 \
    | docker login --username AWS --password-stdin "$registry" >/dev/null
}

case ${1:-} in
  ref) echo "$ref" ;;
  ensure)
    login
    if docker manifest inspect "$ref" >/dev/null 2>&1; then
      echo "toolchain-image: $ref already published"
      exit 0
    fi
    docker build --build-arg "TOOLCHAIN_TAG=$tag" -t "$ref" "$dir"
    docker push "$ref"
    ;;
  pull)
    login
    docker pull -q "$ref" >/dev/null
    docker image inspect --format '{{index .RepoDigests 0}}' "$ref"
    ;;
  *) echo "usage: $0 ref|ensure|pull" >&2; exit 2 ;;
esac
