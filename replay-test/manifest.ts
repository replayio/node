interface TestSpec {
  name: string;
  allowRecordingError?: boolean;
  allowUnusable?: boolean;
}

// Tests to run during the replay test suite.
export const TestManifest: TestSpec[] = [
  { name: "async.js" },
  { name: "basic.js" },
  { name: "control_flow.js" },
  { name: "error.js", allowRecordingError: true },
  { name: "exceptions.js" },
  { name: "napi.js" },
  { name: "objects.js" },
  { name: "run_worker.js" },
  { name: "spawn.js" },
];

// Patterns to ignore in tests in the regular node test suite.
export const NodeTestIgnoreList = [
  // Experimental node features.
  "test-vm-module",
  "wasi",

  // Tanks performance for some reason.
  "test-init.js",
];
