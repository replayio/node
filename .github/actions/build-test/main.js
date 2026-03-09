
const {
  getLatestRevision,
  sendBuildTestRequest,
  newTask,
} = require("../utils");

const revision = getLatestRevision();

const clobberInput = process.env.INPUT_CLOBBER;
console.log("Clobber", clobberInput);
const clobber = clobberInput == "true";

sendBuildTestRequest({
  name: `Node Build/Test ${revision}`,
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
      revision,
      clobber,
    },
    platform
  );

  const testTask = newTask(
    `Test Node ${platform}`,
    {
      kind: "RunTestSuite",
      runtime: "node",
      revision,
    },
    platform,
    [buildTask]
  );

  const jestTask = newTask(
    `Run random Jest tests ${platform}`,
    {
      kind: "JestTests",
      revision,
    },
    platform,
    [buildTask]
  );

  return [buildTask, testTask, jestTask];
}
