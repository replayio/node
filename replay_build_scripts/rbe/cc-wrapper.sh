#!/bin/bash
# CC/CXX for RBE node builds (see build-rbe.py): cc-wrapper.sh <clang|clang++> args...
# Compiles of source files go through rewrapper while build-rbe.py has reproxy
# up; everything else (configure probes, links) runs the same clang locally.
# Both use the hermetic sysroot so local and remote outputs match.
tool=$1
shift
root=$(dirname "$(dirname "$(dirname "$(readlink -f "$0")")")")
tc=$root/.rbe
# V8 9.x trips this clang 16 default-error (BitField::kMax casts); upstream V8
# fixed it much later. Only the RBE toolchain is clang, so silence it here.
flags=(-Wno-enum-constexpr-conversion)

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
  link=
  case " $* " in *" -c "*|*" -E "*|*" -S "*) ;; *) link=-fuse-ld=lld ;; esac
  exec "$tc/llvm/bin/$tool" --sysroot="$tc/sysroot" "${flags[@]}" $link "$@"
fi

# Remote workers see the checkout at a different path: only relative paths.
rel=$(realpath --relative-to="$PWD" "$root")
args=()
for a; do args+=("${a//$root\//$rel/}"); done
# The input processor finds sysroot C headers but not libstdc++'s (Chromium,
# whose configs this mirrors, builds against its own libc++), so ship them,
# plus crtbegin.o: clang only detects the sysroot's GCC install (which adds the
# libstdc++ include dirs) when that file exists. Paths are exec_root-relative.
sr=.rbe/sysroot
inputs=$sr/usr/lib/gcc/x86_64-linux-gnu/10/crtbegin.o,$sr/usr/include/c++/10,$sr/usr/include/x86_64-linux-gnu/c++/10
exec "$tc/reclient/rewrapper" -cfg="$tc/rewrapper.cfg" -exec_root="$root" -inputs="$inputs" \
  "$rel/.rbe/llvm/bin/$tool" --sysroot="$rel/.rbe/sysroot" "${flags[@]}" "${args[@]}"
