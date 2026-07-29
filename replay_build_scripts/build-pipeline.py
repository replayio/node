#!/usr/bin/env python3
"""PipelineUpload for runtime-node-build-and-test (Chromium twin of build-pipeline.py).

Bakes REPLAY_BACKEND_REV into .buildkite/runtime-node-build-and-test.yml and
prints the graph for `buildkite-agent pipeline upload`.

DriverBuildCheck: if linux-recordreplay-<rev12>.tgz is already on S3, replace the
build-driver-linker trigger with a noop (same key) so depends_on stays valid.

In CI (skip-checkout), fetches pin + YAML from public raw.githubusercontent.com
using BUILDKITE_COMMIT. Locally, reads from the node checkout.

Do not curl private replayio/backend for pipeline YAML.
"""

import os
import urllib.error
import urllib.request
from pathlib import Path

PIN_TOKEN = "__REPLAY_BACKEND_REV__"
REPO = "replayio/node"


def read_commit_hash(text: str) -> str:
    return text.strip().split()[0]


def fetch_raw(path: str) -> str:
    commit = os.environ["BUILDKITE_COMMIT"]
    url = f"https://raw.githubusercontent.com/{REPO}/{commit}/{path}"
    with urllib.request.urlopen(url) as resp:
        return resp.read().decode()


def read_text(rel_path: str) -> str:
    local = Path(__file__).resolve().parent.parent / rel_path
    if local.is_file():
        return local.read_text()
    return fetch_raw(rel_path)


def driver_archive_present(driver_revision: str) -> bool:
    """DriverBuildCheck: True if linux driver archive for this rev is already on S3."""
    url = f"https://static.replay.io/downloads/linux-recordreplay-{driver_revision}.tgz"
    req = urllib.request.Request(url, method="HEAD")
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            return 200 <= resp.status < 300
    except (urllib.error.HTTPError, urllib.error.URLError, TimeoutError):
        return False


def apply_driver_build_check(yaml_text: str, pin: str) -> str:
    """Replace build-driver-linker trigger with noop when archive already on S3."""
    driver_revision = pin[:12]
    if not driver_archive_present(driver_revision):
        return yaml_text

    trigger = f"""  - trigger: "build-driver-linker"
    key: "build-driver-linker"
    build:
      commit: "{pin}"
      message: "Triggered from node: ${{BUILDKITE_MESSAGE}}"
"""
    noop = f"""  - label: "DriverBuildCheck (archive present)"
    key: "build-driver-linker"
    agents:
      - "deploy=true"
    plugins:
      - thedyrt/skip-checkout#v0.1.1: ~
    command: echo "DriverBuildCheck: linux-recordreplay-{driver_revision}.tgz already on S3"
"""
    if trigger not in yaml_text:
        raise RuntimeError("DriverBuildCheck: expected build-driver-linker trigger block missing")
    return yaml_text.replace(trigger, noop, 1)


def main() -> None:
    pin = read_commit_hash(read_text("REPLAY_BACKEND_REV"))
    yaml_text = read_text(".buildkite/runtime-node-build-and-test.yml")
    # Bake pin so upload does not depend on REPLAY_BACKEND_REV in the agent env.
    # Leave ${BUILDKITE_*} for Buildkite interpolation at upload time.
    yaml_text = yaml_text.replace(PIN_TOKEN, pin)
    print(apply_driver_build_check(yaml_text, pin))


if __name__ == "__main__":
    main()
