const fs = require("fs");
const os = require("os");
const { spawnSync } = require("child_process");
const node = __dirname;

// Download the latest record/replay driver.
const driverArchive = `${currentPlatform()}-recordreplay.tgz`;
const driverFile = `${currentPlatform()}-recordreplay.${driverExtension()}`;
const driverJSON = `${currentPlatform()}-recordreplay.json`;
spawnChecked("curl", [`https://static.replay.io/downloads/${driverArchive}`, "-o", driverArchive], { stdio: "inherit" });
spawnChecked("tar", ["xf", driverArchive]);
fs.unlinkSync(driverArchive);

// Embed the driver in the source.
const driverContents = fs.readFileSync(driverFile);
const { revision: driverRevision, date: driverDate } = JSON.parse(fs.readFileSync(driverJSON, "utf8"));
fs.unlinkSync(driverFile);
fs.unlinkSync(driverJSON);
let driverString = "";
for (let i = 0; i < driverContents.length; i++) {
  driverString += `\\${driverContents[i].toString(8)}`;
}
fs.writeFileSync(
  `${node}/src/node_record_replay_driver.cc`,
  `
namespace node {
  char gRecordReplayDriver[] = "${driverString}";
  int gRecordReplayDriverSize = ${driverContents.length};
  char gBuildId[] = "${computeBuildId()}";
}
`
);

const numCPUs = os.cpus().length;

if (process.platform == "linux") {
  // Do the build inside a container, to ensure a consistent result
  // with the right glibc dependencies and so forth.
  if (process.env.BUILD_NODE_CONTAINER) {
    spawnChecked(
      "docker",
      ["build", ".", "-f", `${node}/Dockerfile.build`, "-t", "node-build"],
      { stdio: "inherit" }
    );
  }
  spawnChecked(
    "docker",
    ["run", "-v", `${node}:/node`, "-e", "RECORD_REPLAY_DRIVER=0", "node-build"],
    { stdio: "inherit" }
  );
} else {
  if (process.env.CONFIGURE_NODE) {
    spawnChecked(`${node}/configure`, [], { cwd: node, stdio: "inherit" });
  }
  spawnChecked("make", [`-j${numCPUs}`, "-C", "out", "BUILDTYPE=Release"], {
    cwd: node,
    stdio: "inherit",
    env: {
      ...process.env,
      // Disable recording when node runs as part of its compilation process.
      RECORD_REPLAY_DRIVER: "0",
    },
  });
}

function spawnChecked(cmd, args, options) {
  const prettyCmd = [cmd].concat(args).join(" ");
  console.error(prettyCmd);

  const rv = spawnSync(cmd, args, options);

  if (rv.status != 0 || rv.error) {
    console.error(rv.error);
    throw new Error(`Spawned process failed with exit code ${rv.status}`);
  }

  return rv;
}

function currentPlatform() {
  switch (process.platform) {
    case "darwin":
      return "macOS";
    case "linux":
      return "linux";
    default:
      throw new Error(`Platform ${process.platform} not supported`);
  }
}

function driverExtension() {
  return currentPlatform() == "windows" ? "dll" : "so";
}

function computeBuildId() {
  const nodeRevision = spawnChecked("git", ["rev-parse", "--short", "HEAD"]).stdout.toString().trim();
  const nodeDate = spawnChecked("git", [
    "show",
    "HEAD",
    "--pretty=%cd",
    "--date=short",
    "--no-patch",
  ])
    .stdout.toString()
    .trim()
    .replace(/-/g, "-");

  // Use the later of the two dates in the build ID.
  const date = +nodeDate >= +driverDate ? nodeDate : driverDate;

  return `${currentPlatform()}-node-${date}-${nodeRevision}-${driverRevision}`;
}
