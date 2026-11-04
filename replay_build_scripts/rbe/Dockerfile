# Toolchain image for RBE node builds: the local runner AND the EngFlow remote
# worker image, so every compile sees the same gcc, headers and libraries no
# matter where it runs. Same base and compiler packages as the make build's
# backend src/build/docker/linux_node_build.Dockerfile (ubuntu:22.04 +
# build-essential/gcc/g++), pinned by digest once pushed.
#
# node 24 needs gcc >= 12.2 (BUILDING.md), and jammy's default is 11.4, which
# cannot compile deps/ada (it wants a constexpr std::string). gcc 12 comes from
# jammy's own archive and becomes the default compiler, so TOOLCHAIN_PROBE,
# cc-wrapper.sh and the links all see the same one.
#
# Published by toolchain-image.sh as public.ecr.aws/z3x5l6b1/node-rbe-toolchain:
# <hash of this file>; any change here produces a new image (and remote cache
# keys). It is PUBLIC: never COPY repo contents or credentials into it.
FROM ubuntu:22.04@sha256:5ec03bb3441e8b0bf3b4f9cd4629a1ae763010dc3035bb8da3ae6cf026486401

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential make g++ gcc g++-12 gcc-12 \
      ca-certificates curl git python3 python-is-python3 nodejs awscli && \
    rm -rf /var/lib/apt/lists/*
RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-12 100 \
      --slave /usr/bin/g++ g++ /usr/bin/g++-12 \
      --slave /usr/bin/gcov gcov /usr/bin/gcov-12
RUN git config --system --add safe.directory '*'
# The user EngFlow workers run actions as (same as backend's engflow-builder image).
RUN groupadd --gid 114 engflow && \
    useradd --home-dir /home/engflow --create-home --uid 108 --gid 114 engflow

# build-rbe.py checks it runs in the image matching this Dockerfile.
ARG TOOLCHAIN_TAG
RUN test -n "$TOOLCHAIN_TAG" && echo "$TOOLCHAIN_TAG" > /etc/node-rbe-toolchain
