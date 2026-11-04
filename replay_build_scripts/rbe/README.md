# Building node with RBE

Node compiles remotely on EngFlow with reclient, like Replay's Chromium fork:
compiles run on EngFlow's workers, while configure, host tools (torque,
mksnapshot, ...) and links run locally. `node build.js` does everything
(`build-rbe.py`); there is no make build anymore.

## Local builds

You need the EngFlow client certificate and key (AWS Secrets Manager
`engflow-certificate` / `engflow-key`) saved to files:

```sh
export ENGFLOW_CERT_FILE=/path/to/engflow.crt ENGFLOW_KEY_FILE=/path/to/engflow.key
```

(Chromium's `RBE_tls_client_auth_cert` / `RBE_tls_client_auth_key` work too.)

Then, from the node checkout (a git worktree is fine):

```sh
node build.js                                    # on the host, like autoninja
replay_build_scripts/rbe/build-in-container.sh   # in the toolchain image
```

A changed `Dockerfile` has no published image for the workers to pull, so remote
execution and CI stay broken for that branch until someone publishes it (see
below). Until then, build without remote execution, which needs no EngFlow cert:

```sh
REPLAY_RBE_LOCAL=1 replay_build_scripts/rbe/build-in-container.sh
```

## Publishing the toolchain image

The image's tag is a hash of the `Dockerfile`, so each branch pins its own and
all of them coexist in the repository: a node 22 checkout, a node 24 checkout
and a node 26 checkout each resolve a different tag, for their own local
container builds, for the worker image in the reclient platform property, and
for CI's `linux-node-build` base. An existing tag is never pushed over, and
`build-rbe.py` refuses to build a checkout inside an image whose
`/etc/node-rbe-toolchain` is a different tag, so the versions cannot drift into
each other's images.

Publishing is a manual step, from a checkout of the branch that changed the
`Dockerfile`, with an AWS identity that has ECR Public push rights:

```sh
replay_build_scripts/rbe/toolchain-image.sh ensure
```

It builds the image, pushes it under its tag, and does nothing if that tag is
already there. Afterwards `toolchain-image.sh digest` resolves on any host, and
`node build.js` and CI work on that branch.

On the host, the build first checks that your gcc, g++, binutils and glibc are
identical to the EngFlow workers' (see `Dockerfile`, which differs per node
version) and stops otherwise: local and remote steps must use the same
compiler. Use `build-in-container.sh` on other hosts (it needs docker; the
checkout is mounted at its own path, so both commands share `out/`).

Both take the usual `build.js` environment: `DRIVER_REVISION=<rev>` to use the
driver of another backend revision (e.g. when `REPLAY_BACKEND_REV`'s isn't
published yet), `REPLAY_LOCAL_DRIVER_DIR=<dir>` for a locally built one.
`REPLAY_RBE_REQUIRE_REMOTE=1` (set in CI) fails the build if any compile fell
back to local execution.

Output: `out/Release/node`. Rebuilds are incremental (ninja); configure runs
every time but leaves unchanged outputs untouched, so it doesn't trigger
rebuilds by itself.

## Pieces

- `Dockerfile`: the toolchain image, which EngFlow's workers run (pinned by
  digest). Published to `public.ecr.aws/z3x5l6b1/node-rbe-toolchain` by
  `toolchain-image.sh ensure` under a hash of the Dockerfile, so every node
  version has its own. It is public: never copy repo contents or credentials
  into it.
- `build-rbe.py`: downloads reclient and ninja, writes the reclient configs,
  configures with ninja, runs the build with reproxy.
- `cc-wrapper.sh`: the CC/CXX that sends compiles through rewrapper.
- `toolchain.json`, `chromium_cfgs/`: reclient/ninja versions and reclient
  configs, taken from the Chromium fork by `update-toolchain.py <chromium>/src`.

CI builds in backend's `linux-node-build` image, built FROM the toolchain image,
and deploys from there (backend `scripts/docker/`, `src/build/buildNode.ts`).
