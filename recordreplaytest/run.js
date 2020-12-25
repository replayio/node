// Harness for running record/replay tests.
// Run this from the toplevel node directory.

const fs = require("fs");
const { spawn, spawnSync } = require("child_process");
const { findGeckoPath, createTestScript, tmpFile } = require("../../devtools/test/utils");

const startTime = Date.now();

const tests = [
  { example: "basic.js", script: "node_console-01.js" },
  { example: "objects.js", script: "node_object_preview-01.js" },
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

let failures = [];

function elapsedTime() {
  return (Date.now() - startTime) / 1000;
}

async function runTests() {
  for (const test of tests) {
    await runTest(test);
  }

  if (failures.length) {
    console.log(`[${elapsedTime()}] Had ${failures.length} test failures.`);
    failures.forEach(failure => console.log(failure));
  } else {
    console.log(`[${elapsedTime()}] All tests passed.`);
  }

  process.exit(failures.length ? 1 : 0);
}
runTests();

async function runTest({ example, script }, timeout = 120) {
  console.log(`[${elapsedTime()}] Starting test ${example} ${script}`);

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

  const profileArgs = [];
  if (!process.env.NORMAL_PROFILE) {
    const profile = tmpFile();
    fs.mkdirSync(profile);
    profileArgs.push("-profile", profile);
  }

  const testScript = createTestScript({ path: "recordreplaytest/harness.js" });

  const geckoPath = findGeckoPath();

  const url = `http://localhost:8080/view?id=${recordingId}&dispatch=${dispatch}&test=${script}`;
  const gecko = spawn(
    geckoPath,
    ["-foreground", ...profileArgs],
    {
      env: {
        ...process.env,
        MOZ_CRASHREPORTER_AUTO_SUBMIT: "1",
        RECORD_REPLAY_TEST_SCRIPT: testScript,
        RECORD_REPLAY_TEST_URL: url,
        RECORD_REPLAY_SERVER: dispatch,
        // This needs to be set in order for the test script to send messages
        // to the UI process after the test finishes, but isn't otherwise used.
        RECORD_REPLAY_LOCAL_TEST: "1",
      },
    }
  );

  let passed = false;

  function processOutput(data) {
    if (/TestPassed/.test(data.toString())) {
      passed = true;
    }
    process.stdout.write(data);
  }

  gecko.stdout.on("data", processOutput);
  gecko.stderr.on("data", processOutput);

  let resolve;
  const promise = new Promise(r => (resolve = r));

  // FIXME common up with devtools/test/run.js

  let timedOut = false;
  let closed = false;
  gecko.on("close", code => {
    closed = true;
    if (!timedOut) {
      if (code) {
        logFailure(`Exited with code ${code}`);
      } else if (!passed) {
        logFailure("Exited without passing test");
      }
    }
    resolve();
  });

  if (!process.env.RECORD_REPLAY_NO_TIMEOUT) {
    setTimeout(() => {
      if (!closed) {
        logFailure("Timed out");
        timedOut = true;
        gecko.kill();
      }
    }, timeout * 1000);
  }

  await promise;

  function logFailure(why) {
    failures.push(`Failed test: ${script} ${why}`);
    console.log(`[${elapsedTime()}] Test failed: ${why}`);

    // Log an error which github will recognize.
    let msg = `::error ::Failure ${script}`;
    spawnSync("echo", [msg], { stdio: "inherit" });
  }
}
