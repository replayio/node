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

On the host, the build first checks that your gcc, g++, binutils and glibc are
identical to the EngFlow workers' (Ubuntu 22.04's packages, see `Dockerfile`)
and stops otherwise: local and remote steps must use the same compiler. Use
`build-in-container.sh` on other hosts (it needs docker; the checkout is mounted
at its own path, so both commands share `out/`).

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
  `toolchain-image.sh ensure` (CI) under a hash of the Dockerfile. It is public:
  never copy repo contents or credentials into it.
- `build-rbe.py`: downloads reclient and ninja, writes the reclient configs,
  configures with ninja, runs the build with reproxy.
- `cc-wrapper.sh`: the CC/CXX that sends compiles through rewrapper.
- `toolchain.json`, `chromium_cfgs/`: reclient/ninja versions and reclient
  configs, taken from the Chromium fork by `update-toolchain.py <chromium>/src`.

CI builds in backend's `linux-node-build` image, built FROM the toolchain image,
and deploys from there (backend `scripts/docker/`, `src/build/buildNode.ts`).
