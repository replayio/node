#!/usr/bin/env python3
"""PipelineUpload for runtime-node-build-and-test (Chromium twin of build-pipeline.py).

Bakes REPLAY_BACKEND_REV into .buildkite/runtime-node-build-and-test.yml and
prints the graph for `buildkite-agent pipeline upload`.

In CI (skip-checkout), fetches pin + YAML from public raw.githubusercontent.com
using BUILDKITE_COMMIT. Locally, reads from the node checkout.

Do not curl private replayio/backend for pipeline YAML.
"""

import os
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


def main() -> None:
    pin = read_commit_hash(read_text("REPLAY_BACKEND_REV"))
    yaml_text = read_text(".buildkite/runtime-node-build-and-test.yml")
    # Bake pin so upload does not depend on REPLAY_BACKEND_REV in the agent env.
    # Leave ${BUILDKITE_*} for Buildkite interpolation at upload time.
    print(yaml_text.replace(PIN_TOKEN, pin))


if __name__ == "__main__":
    main()
