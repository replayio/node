// Script for building node with support for the record/replay driver.

const fs = require("fs");
const os = require("os");
const { spawnSync } = require("child_process");

// Generate a new build ID.
const buildId = `macOS-node-${makeDate()}-${makeRandomId()}`;

fs.writeFileSync(
  "src/node_build_id.cc",
  `namespace node { char gBuildId[] = "${buildId}"; }`
);

const numCPUs = os.cpus().length;

spawnSync("make", [
  `-j${numCPUs}`,
  "-C",
  "out",
  "BUILDTYPE=Release",
], { stdio: "inherit" });

function makeDate() {
  const now = new Date;
  const year = now.getFullYear();
  const month = (now.getMonth() + 1).toString().padStart(2, "0");
  const date = now.getDate().toString().padStart(2, "0");
  return `${year}${month}${date}`;
}

function makeRandomId() {
  return Math.round(Math.random() * 1e9).toString();
}
