// Script for building node with support for the record/replay driver.
//
// For now this has to be run like so to pick up npm dependencies in the
// backend repo. This needs to be cleaned up.
//   ts-node --dir ~/recordreplay/backend ~/recordreplay/node/build

const fs = require("fs");
const os = require("os");
const { spawnSync } = require("child_process");
const { buildSymbolsArchive } = require("../backend/src/shared/instanceUtils");

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

const objectDirectory = "out/Release";
const libraries = ["node"];

buildSymbolsArchive(buildId, objectDirectory, libraries);

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
