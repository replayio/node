// Test runner for recording node scripts and replaying the resulting recordings.

// Using require() here to workaround typescript errors.
import * as fs from "fs";
import * as os from "os";
import * as path from "path";
import { spawn } from "child_process";
import { TestManifest, NodeTestIgnoreList } from "./manifest";
import { listAllRecordings, uploadRecording } from "@recordreplay/recordings-cli";
import { defer, Deferred, killTransitiveSubprocesses } from "./utils";
import ProtocolClient from "./client";

const Usage = `
Usage: node run <options>
Options:
  --node <path>: Specify the path to the node binary to use to record.
  --api-key <key>: Specify the API key to use for recording and replaying.
  --run-suite: Run the default replay test suite.
  --run-random-tests <count>: Run random tests from node's regular test suite.
  --run-pattern <pattern>: Run all tests matching a pattern.
  --server <address>: Set server to connect to (default wss://dispatch.replay.io).
`;

function doExit(code: number) {
  // Kill any lingering subprocesses before exiting.
  killTransitiveSubprocesses();
  process.exit(code);
}

function bailout(message: string) {
  console.log(message);
  doExit(1);
}

if (process.argv.length == 2) {
  bailout(Usage);
}

let gNodePath: string;
let gAPIKey: string;
let gRunSuite: boolean;
let gRunRandomTests: number;
let gRunPattern: string;
let gDispatchAddress = "wss://dispatch.replay.io";

for (let i = 2; i < process.argv.length; i++) {
  switch (process.argv[i]) {
    case "--node":
      gNodePath = path.resolve(process.cwd(), process.argv[++i]);
      break;
    case "--api-key":
      gAPIKey = process.argv[++i];
      break;
    case "--run-suite":
      gRunSuite = true;
      break;
    case "--run-random-tests":
      gRunRandomTests = +process.argv[++i];
      break;
    case "--run-pattern":
      gRunPattern = process.argv[++i];
      break;
    case "--server":
      gDispatchAddress = process.argv[++i];
      break;
    default:
      bailout(Usage);
  }
}

if (!gNodePath) {
  bailout("Node path not specified");
}

if (!gAPIKey) {
  bailout("API key not specified");
}

if (!gRunSuite && !gRunRandomTests && !gRunPattern) {
  bailout("No tests specified");
}

process.on("unhandledRejection", error => {
  console.error("ErrorUnhandledRejection", error);
});

const gRecordingDirectory = path.join(os.tmpdir(), `recordings-${(Math.random() * 1e9) | 0}`);

let gNumFailures = 0;

async function main() {
  fs.mkdirSync(gRecordingDirectory);

  try {
    if (gRunSuite) {
      await runTestSuite();
    }

    if (gRunRandomTests) {
      await runRandomTests(gRunRandomTests);
    }

    if (gRunPattern) {
      await runTestsMatchingPattern(gRunPattern);
    }
  } finally {
    fs.rmSync(gRecordingDirectory, { force: true, recursive: true });
  }

  if (gNumFailures) {
    console.error(`Had ${gNumFailures} test failures`);
    doExit(1);
  }

  console.log("All tests passed, exiting");
  doExit(0);
}

main();

async function runTestSuite() {
  for (const { name, allowRecordingError, allowUnusable } of TestManifest) {
    await runSingleTest(
      path.join(__dirname, "examples", name),
      { allowRecordingError, allowUnusable }
    );
  }
}

async function runRandomTests(count: number) {
  const testList = readTests(path.join(__dirname, "..", "test"));
  for (let i = 0; i < count; i++) {
    const testPath = pickRandomTest();
    if (testPath) {
      await runSingleTest(testPath, { allowRecordingError: true, allowUnusable: true });
    }
  }

  function pickRandomTest() {
    const index = (Math.random() * testList.length) | 0;
    const testPath = testList[index];
    if (NodeTestIgnoreList.some(pattern => testPath.includes(pattern))) {
      return null;
    }
    return testPath;
  }
}

async function runTestsMatchingPattern(pattern: string) {
  for (const { name, allowRecordingError, allowUnusable } of TestManifest) {
    const testPath = path.join(__dirname, "examples", name);
    if (testPath.includes(pattern)) {
      await runSingleTest(testPath, { allowRecordingError, allowUnusable });
    }
  }

  const testList = readTests(path.join(__dirname, "..", "test"));
  for (const testPath of testList) {
    if (testPath.includes(pattern)) {
      await runSingleTest(testPath, { allowRecordingError: true, allowUnusable: true });
    }
  }
}

// Get the full paths to all JS files in a directory.
function readTests(directory: string): string[] {
  const rv: string[] = [];
  const entries = fs.readdirSync(directory, { withFileTypes: true });
  for (const entry of entries) {
    if (entry.name.endsWith(".js")) {
      rv.push(path.join(directory, entry.name));
    }
    if (entry.isDirectory()) {
      const subdirTests = readTests(path.join(directory, entry.name));
      rv.push(...subdirTests);
    }
  }
  return rv;
}

function logMessage(message: string) {
  console.log((new Date).toISOString(), message);
}

// Spec describing allowed results from a recording process.
interface FailureSpec {
  // Whether the recording process is allowed to exit with an abnormal code.
  allowRecordingError?: boolean;

  // Whether the recording is allowed to be unusable.
  allowUnusable?: boolean;
}

function recordingFailed(
  code: number,
  status: string,
  failureSpec: FailureSpec,
  testPath: string
) {
  if ((code != 0 || status) && !failureSpec.allowRecordingError) {
    return true;
  }
  const recording = lastMatchingRecording(testPath);
  if (!recording || recording.status == "crashed") {
    return true;
  }
  if (recording.unusableReason && !failureSpec.allowUnusable) {
    return true;
  }
  return false;
}

async function runSingleTest(path: string, failureSpec: FailureSpec) {
  logMessage(`StartingTest ${path}`);

  try {
    const child = spawn(
      gNodePath,
      [path],
      {
        stdio: "inherit",
        env: {
          ...process.env,
          RECORD_REPLAY_VERBOSE: "1",
          RECORD_REPLAY_DIRECTORY: gRecordingDirectory,
          RECORD_REPLAY_JS_ASSERTS: "1",
        },
      }
    );

    const exitWaiter: Deferred<{ code: number, status: string }> = defer();

    child.on("close", (code, status) => exitWaiter.resolve({ code, status }));
    setTimeout(() => exitWaiter.resolve({ code: 1, status: "Timed out" }), 120_000);

    const { code, status } = await exitWaiter.promise;

    if (recordingFailed(code, status, failureSpec, path)) {
      logMessage(`TestFailed: Error while recording ${code} ${status}`);
      gNumFailures++;
      return;
    }

    const recordingId = await uploadTestRecording(path);
    if (!recordingId) {
      logMessage(`TestFailed: Could not find recording ID`);
      gNumFailures++;
      return;
    }

    logMessage(`Found recording ID ${recordingId}`);

    const replayErrorWaiter: Deferred<string> = defer();
    replayRecording(recordingId).then(error => replayErrorWaiter.resolve(error));
    setTimeout(() => replayErrorWaiter.resolve("Timed out"), 120_000);

    const replayError = await replayErrorWaiter.promise;
    if (replayError) {
      logMessage(`TestFailed: Replaying recording failed: ${replayError}`);
      gNumFailures++;
      return;
    }

    logMessage(`TestPassed`);
  } catch (e) {
    logMessage(`TestFailed: Exception thrown ${e} ${e.stack}`);
    gNumFailures++;
  }
}

// Get info for the last recording created for the given test path.
function lastMatchingRecording(testPath: string) {
  const recordings = listAllRecordings({ directory: gRecordingDirectory });
  for (let i = recordings.length - 1; i >= 0; i--) {
    const recording = recordings[i];
    const argv = recording.metadata?.argv;
    if (argv && argv.some(arg => arg.includes(testPath))) {
      return recording;
    }
  }
  return null;
}

async function uploadTestRecording(testPath: string): Promise<string | null> {
  const recording = lastMatchingRecording(testPath);
  if (recording) {
    return uploadRecording(
      recording.id,
      { directory: gRecordingDirectory, server: gDispatchAddress, apiKey: gAPIKey }
    );
  }
  return null;
}

async function replayRecording(recordingId: string): Promise<string | null> {
  const client = new ProtocolClient(gDispatchAddress, {
    onError: e => logMessage(`Socket error ${e}`),
    onClose: (code, reason) => logMessage(`Socket closed ${code} ${reason}`),
  });
  await client.waitUntilOpen();

  const testErrorWaiter: Deferred<string | null> = defer();

  client.addEventListener("Session.unprocessedRegions", () => {});
  client.addEventListener("Recording.sessionError", e => {
    testErrorWaiter.resolve(`Session error ${JSON.stringify(e)}`);
  });

  try {
    const result = await client.sendCommand("Recording.createSession", {
      recordingId,
    });

    const { sessionId } = result;
    logMessage(`Created session ${sessionId}`);

    client.sendCommand(
      "Session.ensureProcessed",
      { level: "basic" },
      sessionId
    ).then(() => testErrorWaiter.resolve(null));

    const error = await testErrorWaiter.promise;
    return error;
  } finally {
    client.close();
  }
}
