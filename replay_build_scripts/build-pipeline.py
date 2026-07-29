#!/usr/bin/env python3
"""PipelineUpload for runtime-node-build-and-test (Chromium twin of build-pipeline.py).

Reads REPLAY_BACKEND_REV from the node checkout, bakes the pin into
.buildkite/runtime-node-build-and-test.yml, and prints the graph for
`buildkite-agent pipeline upload`.

Do not curl private replayio/backend for pipeline YAML.
"""

from pathlib import Path

PIN_TOKEN = "__REPLAY_BACKEND_REV__"


def read_commit_hash(file_path: Path) -> str:
    return file_path.read_text().strip().split()[0]


def main() -> None:
    root = Path(__file__).resolve().parent.parent
    pin = read_commit_hash(root / "REPLAY_BACKEND_REV")
    yaml_path = root / ".buildkite" / "runtime-node-build-and-test.yml"
    # Bake pin so upload does not depend on REPLAY_BACKEND_REV in the agent env.
    # Leave ${BUILDKITE_*} for Buildkite interpolation at upload time.
    print(yaml_path.read_text().replace(PIN_TOKEN, pin))


if __name__ == "__main__":
    main()
