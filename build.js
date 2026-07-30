const fs = require("fs");
const os = require("os");
const path = require("path");
const { spawnSync } = require("child_process");
const node = __dirname;
const OutDir = path.join(node, "out");

// Use local driver directory if provided, otherwise download from S3.
const localDriverDir = process.env.REPLAY_LOCAL_DRIVER_DIR;
const driverFile = `${currentPlatform()}-recordreplay.${driverExtension()}`;
const driverJSON = `${currentPlatform()}-recordreplay.json`;

let driverContents;
let driverRevision;
let driverDate;

if (localDriverDir) {
  console.log("[build] Loading driver from local directory:", localDriverDir);
  driverContents = fs.readFileSync(path.join(localDriverDir, driverFile));
  const driverInfo = JSON.parse(fs.readFileSync(path.join(localDriverDir, driverJSON), "utf8"));
  driverRevision = driverInfo.revision;
  driverDate = driverInfo.date;
} else {
  console.log("[build] Downloading driver from S3...");
  let driverArchive = `${currentPlatform()}-recordreplay.tgz`;
  let downloadDriverRevision = process.env.DRIVER_REVISION ? process.env.DRIVER_REVISION : fs.readFileSync("REPLAY_BACKEND_REV", "utf8");
  let downloadArchive = `${currentPlatform()}-recordreplay-${downloadDriverRevision.trim().substring(0, 12)}.tgz`;
  let downloadUrl = `https://static.replay.io/downloads/${downloadArchive}`;
  const driverArchivePath = path.join(OutDir, driverArchive);
  downloadDriverArchive(downloadUrl, driverArchivePath);
  spawnChecked("tar", ["xf", driverArchivePath, "-C", OutDir]);
  fs.unlinkSync(driverArchivePath);

  driverContents = fs.readFileSync(path.join(OutDir, driverFile));
  const driverInfo = JSON.parse(fs.readFileSync(path.join(OutDir, driverJSON), "utf8"));
  driverRevision = driverInfo.revision;
  driverDate = driverInfo.date;
  fs.unlinkSync(path.join(OutDir, driverFile));
  fs.unlinkSync(path.join(OutDir, driverJSON));
  fs.unlinkSync(path.join(OutDir, `${driverFile}.symbols.json`));
}
console.log("[build] Generating driver source file...");
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

function getSanitizedEnv() {
  const env = { ...process.env };
  if (env.PATH) {
    env.PATH = env.PATH.split(":").filter(p => !p.includes("/nix/")).join(":");
  }
  delete env.NIX_PROFILES;
  delete env.NIX_SSL_CERT_FILE;
  return env;
}

const buildEnv = getSanitizedEnv();

if (process.env.CONFIGURE_NODE) {
  console.log("[build] Running configure...");
  spawnChecked(`${node}/configure`, [], { cwd: node, stdio: "inherit", env: buildEnv });
}
console.log("[build] Running make...");
spawnChecked("make", [`-j${numCPUs}`, "-C", OutDir, "BUILDTYPE=Release"], {
  cwd: node,
  stdio: "inherit",
  env: {
    ...buildEnv,
    RECORD_REPLAY_DONT_RECORD: "1",
  },
});

function downloadDriverArchive(downloadUrl, driverArchivePath) {
  curl(downloadUrl, driverArchivePath);
}

function curl(url, outputPath) {
  const prettyCmd = ["curl", "--fail", url, "-o", outputPath].join(" ");
  console.error(prettyCmd);

  const rv = spawnSync("curl", ["--fail", url, "-o", outputPath], {
    stdio: "inherit",
  });

  if (rv.status != 0 || rv.error) {
    console.error(rv.error);
    throw new Error(`Target driver/linker was not found: ${url}`);
  }
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

/**
 * @returns {string} "YYYYMMDD" format of UTC timestamp of given revision.
 */
function getRevisionDate(
  revision = "HEAD",
  spawnOptions
) {
  const dateString = spawnChecked(
    "git",
    ["show", revision, "--pretty=%cd", "--date=iso-strict", "--no-patch"],
    spawnOptions
  )
    .stdout.toString()
    .trim();

  // convert to UTC -> then get the date only
  // explanations: https://github.com/replayio/backend/pull/7115#issue-1587869475
  return new Date(dateString).toISOString().substring(0, 10).replace(/-/g, "");
}

/**
 * WARNING: We have copy-and-pasted `computeBuildId` into all our runtimes and `backend`.
 * When changing this: always keep all versions of this in sync, or else, builds will break.
 */
function computeBuildId() {
  const runtimeRevision = spawnChecked("git", ["rev-parse", "--short=12", "HEAD"]).stdout.toString().trim();
  const runtimeDate = getRevisionDate();

  // Use the later of the two dates in the build ID.
  const date = +runtimeDate >= +driverDate ? runtimeDate : driverDate;

  // Chromium twin: upload_build_artifacts.mjs buildIdExtension / backend utils.ts.
  const buildIdExtension =
    process.env.BUILDKITE_BRANCH !== process.env.BUILDKITE_PIPELINE_DEFAULT_BRANCH
      ? "-dev"
      : process.env.LOCAL_DEVELOPER_BUILD_EXTENSION || "";

  return `${currentPlatform()}-node-${date}-${runtimeRevision}-${driverRevision}${buildIdExtension}`;
}
