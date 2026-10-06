#!/bin/bash
# Publish an RBE build like backend `buildNode.ts deploy` publishes make builds
# (binary only, no symbols archive), so it can be tested by build id:
#   https://static.replay.io/downloads/<id>, test-node-recording --build-id <id>
# Temporary: goes away once RBE builds replace make builds and use plain -dev ids.
set -euo pipefail
root=$(dirname "$(dirname "$(dirname "$(readlink -f "$0")")")")
build_id=$(sed -n 's/.*gBuildId\[\] = "\([^"]*\)".*/\1/p' "$root/src/node_record_replay_driver.cc" | head -n1)
# Never touch a make build's id (same commit, same id without the suffix).
case $build_id in
  linux-node-*-rbe-dev) ;;
  *) echo "upload-rbe-build: refusing to upload non-RBE build id '$build_id'" >&2; exit 1 ;;
esac
bucket=${RECORDREPLAY_BUCKET:-recordreplay-us-east-2}
aws s3 cp --acl bucket-owner-full-control "$root/out/Release/node" "s3://$bucket/builds/$build_id"
aws s3 cp "s3://$bucket/builds/$build_id" "s3://recordreplay-website/downloads/$build_id"
echo "BuildUploaded https://static.replay.io/downloads/$build_id"
