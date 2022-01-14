// Test runner for recording node scripts and replaying the resulting recordings.

// Using require() here to workaround typescript errors.
import * as fs from "fs";
import * as path from "path";
import { spawnSync, SpawnSyncReturns } from "child_process";
import { TestManifest, NodeTestIgnoreList } from "./manifest";
import { listAllRecordings, uploadRecording } from "@recordreplay/recordings-cli";
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

function bailout(message) {
  console.log(message);
  process.exit(1);
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

const gRecordingDirectory = path.join(__dirname, `recordings-${(Math.random() * 1e9) | 0}`);

let gNumFailures = 0;

async function main() {
  fs.mkdirSync(gRecordingDirectory);

  if (gRunSuite) {
    await runTestSuite();
  }

  if (gRunRandomTests) {
    await runRandomTests(gRunRandomTests);
  }

  if (gRunPattern) {
    await runTestsMatchingPattern(gRunPattern);
  }

  fs.rmSync(gRecordingDirectory, { force: true, recursive: true });

  if (gNumFailures) {
    console.error(`Had ${gNumFailures} test failures`);
    process.exit(1);
  }

  console.log("All tests passed, exiting");
  process.exit(0);
}

main();

async function runTestSuite() {
  for (const { name, allowRecordingError } of TestManifest) {
    await runSingleTest(path.join(__dirname, "examples", name), allowRecordingError);
  }
}

async function runRandomTests(count: number) {
  const testList = readTests(path.join(__dirname, "..", "test"));
  for (let i = 0; i < count; i++) {
    const testPath = pickRandomTest();
    if (testPath) {
      await runSingleTest(testPath, true);
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
  for (const { name, allowRecordingError } of TestManifest) {
    const testPath = path.join(__dirname, "examples", name);
    if (testPath.includes(pattern)) {
      await runSingleTest(testPath, allowRecordingError);
    }
  }

  const testList = readTests(path.join(__dirname, "..", "test"));
  for (const testPath of testList) {
    if (testPath.includes(pattern)) {
      await runSingleTest(testPath, true);
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

function recordingFailed(
  rv: SpawnSyncReturns<Buffer>,
  allowRecordingError: boolean,
  testPath: string
) {
  if ((rv.status != 0 || rv.error) && !allowRecordingError) {
    return true;
  }
  // If the recording is unusable or crashed, the test failed.
  const recording = lastMatchingRecording(testPath);
  if (!recording || recording.unusableReason || recording.status == "crashed") {
    return true;
  }
  return false;
}

async function runSingleTest(path: string, allowRecordingError: boolean) {
  logMessage(`StartingTest ${path}`);

  try {
    const rv = spawnSync(
      gNodePath,
      [path],
      {
        stdio: "inherit",
        env: {
          ...process.env,
          RECORD_REPLAY_VERBOSE: "1",
          RECORD_REPLAY_DIRECTORY: gRecordingDirectory,
        },
      }
    );

    if (recordingFailed(rv, allowRecordingError, path)) {
      logMessage(`TestFailed: Error while recording ${rv.status} ${rv.error}`);
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

    const passed = await replayRecording(recordingId);
    if (!passed) {
      logMessage(`TestFailed: Replaying recording failed`);
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

async function replayRecording(recordingId: string): Promise<boolean> {
  const client = new ProtocolClient(gDispatchAddress, {
    onError: e => logMessage(`Socket error ${e}`),
    onClose: (code, reason) => logMessage(`Socket closed ${code} ${reason}`),
  });
  await client.waitUntilOpen();

  let testPassed = true;

  client.addEventListener("Session.unprocessedRegions", () => {});
  client.addEventListener("Recording.sessionError", e => {
    logMessage(`Session error ${JSON.stringify(e)}`);
    testPassed = false;
  });

  try {
    const result = await client.sendCommand("Recording.createSession", {
      recordingId,
    });

    const { sessionId } = result;
    logMessage(`Created session ${sessionId}`);

    await client.sendCommand(
      "Session.ensureProcessed",
      { level: "executionIndexed" },
      sessionId
    );
  } finally {
    client.close();
  }

  return testPassed;
}
