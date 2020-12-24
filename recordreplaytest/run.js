// Harness for running record/replay tests.

const os = require("os");
const fs = require("fs");
const { spawn, spawnSync } = require("child_process");
const { findGeckoPath } = require("../../devtools/test/utils");

const tests = [
  { example: "basic.js", script: "node_console-01.js" },
];

if (!process.env.RECORD_REPLAY_DRIVER) {
  console.log("RECORD_REPLAY_DRIVER not set, exiting.");
  process.exit(1);
}

if (!process.env.RECORD_REPLAY_DISPATCH) {
  console.log("RECORD_REPLAY_DISPATCH not set, exiting.");
  process.exit(1);
}
const dispatch = process.env.RECORD_REPLAY_DISPATCH;

function tmpFile() {
  return os.tmpdir() + "/" + ((Math.random() * 1e9) | 0);
}

async function runTests() {
  for (const test of tests) {
    await runTest(test);
  }
}
runTests();

async function runTest({ example, script }) {
  const recordingIdFile = tmpFile();
  spawnSync(
    `${__dirname}/../out/Release/node`,
    [`${__dirname}/tests/${example}`],
    {
      env: {
        ...process.env,
        RECORD_REPLAY_RECORDING_ID_FILE: recordingIdFile,
      },
      stdio: "inherit",
    },
  );
  const recordingId = fs.readFileSync(recordingIdFile, "utf8").trim();

  console.log("RECORDING_ID", recordingId);

  const profileArgs = [];
  if (!process.env.NORMAL_PROFILE) {
    const profile = tmpFile();
    fs.mkdirSync(profile);
    profileArgs.push("-profile", profile);
  }

  const geckoPath = findGeckoPath();

  const url = `http://localhost:8080/view?id=${recordingId}&dispatch=${dispatch}&test=${script}`;
  const gecko = spawn(geckoPath, ["-foreground", ...profileArgs, url]);
}
