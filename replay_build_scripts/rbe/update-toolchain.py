#!/usr/bin/env python3
"""Sync the RBE toolchain pins and reclient configs from a Chromium fork checkout.

Node's RBE build runs on the same EngFlow cluster as Chromium, so it uses the
exact same clang, sysroot, reclient, ninja and remote worker image. Nothing
here is hand-edited: rerun this after the Chromium fork rolls any of them.

  python3 replay_build_scripts/rbe/update-toolchain.py <chromium>/src

Writes toolchain.json and copies Chromium's reclient configs unmodified into
chromium_cfgs/ (build-rbe.py only fills in machine-specific paths).
"""

import json
import os
import re
import shutil
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
CFGS = {
    # Chromium's reproxy config (buildtools/reclient_cfgs/reproxy.cfg).
    "reproxy.cfg": "buildtools/reclient_cfgs/reproxy.cfg",
    # The config Chromium's clang compiles go through.
    "rewrapper_linux.cfg": "buildtools/reclient_cfgs/chromium-browser-clang/rewrapper_linux.cfg",
}


def read(src, rel):
  with open(os.path.join(src, rel)) as f:
    return f.read()


def match(pattern, text, what):
  m = re.search(pattern, text, re.M)
  if not m:
    sys.exit(f"update-toolchain: could not find {what}")
  return m.group(1)


def main():
  if len(sys.argv) != 2:
    sys.exit(__doc__)
  src = os.path.abspath(sys.argv[1])

  update_py = read(src, "tools/clang/scripts/update.py")
  clang = "%s-%s" % (
      match(r"^CLANG_REVISION = '([^']+)'", update_py, "CLANG_REVISION"),
      match(r"^CLANG_SUB_REVISION = (\d+)", update_py, "CLANG_SUB_REVISION"))

  sysroot = json.loads(read(src, "build/linux/sysroot_scripts/sysroots.json"))["bullseye_amd64"]

  deps = read(src, "DEPS")
  pins = {
      "chromium_revision": subprocess.check_output(["git", "-C", src, "rev-parse", "HEAD"], text=True).strip(),
      "clang": clang,
      "sysroot": {"tarball": sysroot["Tarball"], "sha1": sysroot["Sha1Sum"]},
      "reclient": match(r"'reclient_version': '([^']+)'", deps, "reclient_version in DEPS"),
      "ninja": match(r"'ninja_version': '([^']+)'", deps, "ninja_version in DEPS"),
  }
  with open(os.path.join(SCRIPT_DIR, "toolchain.json"), "w") as f:
    json.dump(pins, f, indent=2)
    f.write("\n")

  out_dir = os.path.join(SCRIPT_DIR, "chromium_cfgs")
  os.makedirs(out_dir, exist_ok=True)
  for name, rel in CFGS.items():
    shutil.copyfile(os.path.join(src, rel), os.path.join(out_dir, name))

  print(json.dumps(pins, indent=2))


if __name__ == "__main__":
  main()
