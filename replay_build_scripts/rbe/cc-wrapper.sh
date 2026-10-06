#!/bin/bash
# CC/CXX for RBE node builds (see build-rbe.py): cc-wrapper.sh <gcc|g++> args...
# Compiles of source files go through rewrapper while build-rbe.py has reproxy
# up; everything else (configure probes, links) runs locally. Both run the
# toolchain image's /usr/bin/<tool>, so local and remote outputs match.
tool=$1
shift
root=$(dirname "$(dirname "$(dirname "$(readlink -f "$0")")")")
tc=$root/.rbe

remote=
if [ -n "$REPLAY_RBE_REPROXY" ]; then
  compile= src=
  for a; do
    case $a in
      -c) compile=1 ;;
      *.c|*.cc|*.cpp|*.S) src=$a ;;
    esac
  done
  # The generated driver source embeds the whole recorder .so and changes with
  # every commit (build id); uploading it costs more than compiling it here.
  case $src in *node_record_replay_driver.cc) src= ;; esac
  [ -n "$compile" ] && [ -n "$src" ] && remote=1
fi

if [ -z "$remote" ]; then
  exec "/usr/bin/$tool" "$@"
fi

# Remote workers see the checkout at a different path: only relative paths.
rel=$(realpath --relative-to="$PWD" "$root")
args=()
for a; do args+=("${a//$root\//$rel/}"); done
exec "$tc/reclient/rewrapper" -cfg="$tc/rewrapper_linux.cfg" -exec_root="$root" \
  "/usr/bin/$tool" "${args[@]}"
