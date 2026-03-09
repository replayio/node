
const {
  getLatestRevision,
  sendBuildTestRequest,
  spawnChecked,
  newTask,
} = require("../utils");

const revision = getLatestRevision();

sendBuildTestRequest({
  name: `Node Release ${revision}`,
  tasks: [
    ...platformTasks("macOS"),
    ...platformTasks("linux"),
  ],
});

function platformTasks(platform) {
  const releaseTask = newTask(
    `Release Node ${platform}`,
    {
      kind: "ReleaseRuntime",
      runtime: "node",
      revision,
    },
    platform
  );
  return [releaseTask];
}
