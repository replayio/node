
const {
  getLatestRevision,
  sendBuildTestRequest,
  newTask,
} = require("../utils");

const revision = getLatestRevision();

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

  return [buildTask, testTask];
}
