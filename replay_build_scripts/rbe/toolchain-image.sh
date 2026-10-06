#!/bin/bash
# The RBE toolchain image (see Dockerfile next to this script).
#   toolchain-image.sh ref      print public.ecr.aws/f8g4u4l4/node-rbe-toolchain:<tag>
#   toolchain-image.sh ensure   build + push it unless that tag already exists
#                               (AWS identity with ecr-public push rights)
#   toolchain-image.sh pull     pull it and print its digest ref (repo@sha256:...)
# The tag is a hash of the Dockerfile, so a tag is built once and then frozen:
# apt packages float, but every build of a given Dockerfile uses the same image.
# ECR Public so EngFlow can pull it anonymously (like backend's engflow-builder).
set -euo pipefail
dir=$(dirname "$(readlink -f "$0")")
repo=public.ecr.aws/f8g4u4l4/node-rbe-toolchain
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
    # AWS CLI from a container: the agent host may not have one. Uses the
    # agent's AWS identity (instance role), like the build's S3 uploads.
    docker run --rm public.ecr.aws/aws-cli/aws-cli:2.15.0 ecr-public get-login-password --region us-east-1 \
      | docker login --username AWS --password-stdin public.ecr.aws
    docker push "$ref"
    ;;
  pull)
    docker pull -q "$ref" >/dev/null
    docker image inspect --format '{{index .RepoDigests 0}}' "$ref"
    ;;
  *) echo "usage: $0 ref|ensure|pull" >&2; exit 2 ;;
esac
