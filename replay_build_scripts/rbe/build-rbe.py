#!/usr/bin/env python3
"""Prototype: build node with remote execution (reclient + EngFlow), like Chromium.

Compiles run remotely through rewrapper; configure probes, host tool runs
(torque, mksnapshot, ...) and links stay local. The toolchain is hermetic so
remote workers and every checkout produce the same cache keys:

  .rbe/llvm     Chromium's clang package (same pin as the Chromium fork)
  .rbe/sysroot  Chromium's Debian bullseye sysroot (headers + libs to link against)
  .rbe/reclient reclient (rewrapper/reproxy/bootstrap), from CIPD
  .rbe/ninja    ninja, from CIPD

Everything lives under the checkout (rewrapper's exec_root), and compile
command lines only use paths relative to out/Release.

This uses ./configure --ninja, which rewrites config.gypi in the source root,
so it must NOT run in a checkout whose out/ is a make (non-RBE) build. Both
this script and build.js refuse to mix the two (out/.replay-rbe marker).

Usage (from build.js with REPLAY_RBE=1, after the driver source is generated):
  python3 replay_build_scripts/rbe/build-rbe.py [-j N]

Needs the same RBE_* environment Chromium's CI uses (RBE_service,
RBE_tls_client_auth_cert, RBE_tls_client_auth_key, ...).
"""

import argparse
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.request
import zipfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(SCRIPT_DIR))
TC = os.path.join(ROOT, ".rbe")
OUT = os.path.join(ROOT, "out")
BUILD_DIR = os.path.join(OUT, "Release")
MARKER = os.path.join(OUT, ".replay-rbe")

with open(os.path.join(SCRIPT_DIR, "toolchain.json")) as f:
  PINS = json.load(f)


def log(msg):
  print(f"[build-rbe] {msg}", flush=True)


def fetch(url):
  log(f"downloading {url}")
  with urllib.request.urlopen(url) as resp:
    return resp.read()


def install(name, pin_key, extract):
  """Install a toolchain component into .rbe/<name> unless the pin matches."""
  dest = os.path.join(TC, name)
  stamp = os.path.join(TC, f"{name}.stamp")
  want = json.dumps(PINS[pin_key], sort_keys=True)
  if os.path.exists(stamp) and open(stamp).read() == want:
    return
  shutil.rmtree(dest, ignore_errors=True)
  os.makedirs(dest)
  extract(dest)
  with open(stamp, "w") as f:
    f.write(want)


def extract_tar_xz(url, sha1=None):
  def run(dest):
    data = fetch(url)
    if sha1 and hashlib.sha1(data).hexdigest() != sha1:
      raise RuntimeError(f"sha1 mismatch for {url}")
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:xz") as tar:
      tar.extractall(dest)
  return run


def extract_cipd(package, version):
  def run(dest):
    url = f"https://chrome-infra-packages.appspot.com/dl/{package}/+/{version}"
    with zipfile.ZipFile(io.BytesIO(fetch(url))) as z:
      for info in z.infolist():
        z.extract(info, dest)
        mode = info.external_attr >> 16
        if mode:
          os.chmod(os.path.join(dest, info.filename), mode & 0o777)
  return run


def setup_toolchain():
  os.makedirs(TC, exist_ok=True)
  clang = PINS["clang"]
  install("llvm", "clang", extract_tar_xz(
      f"https://commondatastorage.googleapis.com/chromium-browser-clang/Linux_x64/clang-{clang}.tar.xz"))
  sysroot = PINS["sysroot"]
  install("sysroot", "sysroot", extract_tar_xz(
      f"https://commondatastorage.googleapis.com/chrome-linux-sysroot/toolchain/{sysroot['sha1']}/{sysroot['tarball']}",
      sysroot["sha1"]))
  install("reclient", "reclient", extract_cipd("infra/rbe/client/linux-amd64", PINS["reclient"]))
  install("ninja", "ninja", extract_cipd("infra/3pp/tools/ninja/linux-amd64", PINS["ninja"]))

  # configure and gyp bake CC/CXX into build.ninja; point them at the wrapper.
  bin_dir = os.path.join(TC, "bin")
  os.makedirs(bin_dir, exist_ok=True)
  for tool in ("clang", "clang++"):
    path = os.path.join(bin_dir, tool)
    with open(path, "w") as f:
      f.write(f'#!/bin/sh\nexec "{SCRIPT_DIR}/cc-wrapper.sh" {tool} "$@"\n')
    os.chmod(path, 0o755)


def socket_address():
  # unix socket paths are length-limited; key on the checkout so concurrent
  # builds in different checkouts (and Chromium's reproxy) don't collide.
  digest = hashlib.sha256(ROOT.encode()).hexdigest()[:16]
  return f"unix:///tmp/reproxy-node-{digest}.sock"


def write_cfgs():
  reclient = os.path.join(TC, "reclient")
  with open(os.path.join(TC, "reproxy.cfg"), "w") as f:
    f.write("\n".join([
        f"depsscanner_address=exec://{reclient}/scandeps_server",
        "async_reproxy_termination=true",
        "cas_concurrency=1000",
        "compression_threshold=0",
        "deps_cache_max_mb=256",
        "fail_early_min_action_count=4000",
        "fail_early_min_fallback_ratio=0.5",
        "fast_log_collection=true",
        "log_format=reducedtext",
        "max_concurrent_requests_per_conn=50",
        "max_concurrent_streams_per_conn=50",
        "min_grpc_connections=50",
        "use_batches=false",
        "use_unified_uploads=true",
    ]) + "\n")
  with open(os.path.join(TC, "rewrapper.cfg"), "w") as f:
    f.write("\n".join([
        "canonicalize_working_dir=true",
        "dial_timeout=10m",
        "exec_timeout=4m",
        "reclient_timeout=8m",
        "labels=type=compile,compiler=clang,lang=cpp",
        f"platform=container-image={PINS['container_image']},OSFamily=linux",
    ]) + "\n")


def rbe_env():
  logs = os.path.join(BUILD_DIR, ".reproxy_tmp", "logs")
  cache = os.path.join(BUILD_DIR, ".reproxy_tmp", "cache")
  os.makedirs(logs, exist_ok=True)
  os.makedirs(cache, exist_ok=True)
  env = dict(os.environ)
  env.update({
      "RBE_server_address": socket_address(),
      "RBE_output_dir": logs,
      "RBE_proxy_log_dir": logs,
      "RBE_log_dir": logs,
      "RBE_cache_dir": cache,
      # Never race against local compiles: keep local cores free and only fall
      # back when remote execution fails.
      "RBE_exec_strategy": "remote_local_fallback",
      "REPLAY_RBE_REPROXY": "1",
  })
  return env


def check_out_dir():
  if os.path.exists(os.path.join(OUT, "Makefile")) and not os.path.exists(MARKER):
    sys.exit("[build-rbe] out/ holds a make (non-RBE) build; use a separate checkout for RBE builds")
  os.makedirs(OUT, exist_ok=True)
  open(MARKER, "w").close()


def configure():
  # Cheap, and ninja only reruns commands whose command line changed, so
  # always regenerating keeps build.ninja in sync with the .gyp files.
  env = dict(os.environ)
  env["CC"] = os.path.join(TC, "bin", "clang")
  env["CXX"] = os.path.join(TC, "bin", "clang++")
  env.pop("REPLAY_RBE_REPROXY", None)
  subprocess.check_call([sys.executable, "configure.py", "--ninja"], cwd=ROOT, env=env)


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument("-j", type=int, default=200, help="ninja parallelism (mostly remote)")
  args = parser.parse_args()

  check_out_dir()
  setup_toolchain()
  write_cfgs()
  configure()

  reclient = os.path.join(TC, "reclient")
  cfg = os.path.join(TC, "reproxy.cfg")
  env = rbe_env()
  # A stale reproxy from an interrupted build would hold the socket.
  subprocess.call([os.path.join(reclient, "bootstrap"), "--shutdown", f"--cfg={cfg}"], env=env,
                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
  subprocess.check_call([os.path.join(reclient, "bootstrap"),
                         f"--re_proxy={os.path.join(reclient, 'reproxy')}", f"--cfg={cfg}"], env=env)
  start = time.time()
  try:
    rv = subprocess.call([os.path.join(TC, "ninja", "ninja"), "-C", BUILD_DIR, f"-j{args.j}"],
                         cwd=ROOT, env={**env, "RECORD_REPLAY_DONT_RECORD": "1"})
  finally:
    # Prints the remote/cache-hit/fallback summary.
    subprocess.call([os.path.join(reclient, "bootstrap"), "--shutdown", f"--cfg={cfg}"], env=env)
  log(f"ninja finished in {time.time() - start:.0f}s with exit code {rv}")
  sys.exit(rv)


if __name__ == "__main__":
  main()
