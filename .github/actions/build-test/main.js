
const {
  getLatestRevision,
  sendBuildTestRequest,
  spawnChecked,
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
  )

  // FIXME we need a test suite to run as well.
  return [buildTask];
}
