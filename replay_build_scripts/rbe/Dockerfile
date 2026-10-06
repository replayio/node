# Toolchain image for RBE node builds: the local runner AND the EngFlow remote
# worker image, so every compile sees the same gcc, headers and libraries no
# matter where it runs. Same base and compiler packages as the make build's
# backend src/build/docker/linux_node_build.Dockerfile (ubuntu:22.04 +
# build-essential/gcc/g++), pinned by digest once pushed.
#
# Published by toolchain-image.sh as public.ecr.aws/f8g4u4l4/node-rbe-toolchain:
# <hash of this file>; any change here produces a new image (and remote cache
# keys). It is public: never COPY repo contents or credentials into it.
FROM ubuntu:22.04@sha256:5ec03bb3441e8b0bf3b4f9cd4629a1ae763010dc3035bb8da3ae6cf026486401

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential make g++ gcc \
      ca-certificates curl git python3 python-is-python3 nodejs awscli && \
    rm -rf /var/lib/apt/lists/*
RUN git config --system --add safe.directory '*'
# The user EngFlow workers run actions as (same as backend's engflow-builder image).
RUN groupadd --gid 114 engflow && \
    useradd --home-dir /home/engflow --create-home --uid 108 --gid 114 engflow

# build-rbe.py checks it runs in the image matching this Dockerfile.
ARG TOOLCHAIN_TAG
RUN test -n "$TOOLCHAIN_TAG" && echo "$TOOLCHAIN_TAG" > /etc/node-rbe-toolchain
