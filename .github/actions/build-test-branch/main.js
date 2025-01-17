
const {
  getLatestRevision,
  sendBuildTestRequest,
  newTask,
} = require("../utils");

const branchName = process.env.GITHUB_REF_NAME;
console.log("BranchName", branchName);

const nodeRevision = getLatestRevision();

const driverRevision = process.env.INPUT_DRIVER_REVISION;
console.log("DriverRevision", driverRevision);

const clobberInput = process.env.INPUT_CLOBBER;
console.log("Clobber", clobberInput);
const clobber = clobberInput == "true";

const slotInput = process.env.INPUT_SLOT;
console.log("Slot", slotInput);
const slot = slotInput ? +slotInput : undefined;

let requestName = `Node Build/Test Branch ${branchName} ${nodeRevision}`;
if (driverRevision) {
  requestName += ` driver ${driverRevision}`;
}
if (slot) {
  requestName += ` slot ${slot}`;
}

sendBuildTestRequest({
  name: requestName,
  tasks: [
    ...platformTasks("macOS"),
    ...platformTasks("linux"),
  ],
});

function platformTasks(platform) {
  const buildTask = newTask(
    `Build Node ${platform}`,
    {
      kind: "BuildRuntime",
      runtime: "node",
      revision: nodeRevision,
      branch: branchName,
      branchSlot: slot,
      driverRevision,
      clobber,
    },
    platform
  );

  const testTask = newTask(
    `Test Node ${platform}`,
    {
      kind: "RunTestSuite",
      runtime: "node",
      revision: nodeRevision,
      driverRevision,
    },
    platform,
    [buildTask]
  );

  const jestTask = newTask(
    `Run random Jest tests ${platform}`,
    {
      kind: "JestTests",
      revision: nodeRevision,
      driverRevision,
    },
    platform,
    [buildTask]
  );

  return [buildTask, testTask, jestTask];
}
