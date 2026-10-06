# Runner for the RBE node build prototype (build-rbe.py). The compiler toolchain
# is downloaded by build-rbe.py into the checkout; this only needs the tools
# that run around it (configure, build.js, gyp actions, driver download).
FROM ubuntu:22.04
RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      ca-certificates curl git python3 python-is-python3 nodejs xz-utils binutils && \
    rm -rf /var/lib/apt/lists/*
RUN git config --system --add safe.directory '*'
