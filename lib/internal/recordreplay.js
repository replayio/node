'use strict';

// Methods for interacting with the record/replay driver.

function initializeRecordReplay() {
  process._recordReplaySetCommandCallback(commandCallback);
}

const CommandCallbacks = {
  "Target.getCurrentMessageContents": Target_getCurrentMessageContents
};

function commandCallback(method, params) {
  console.debugLog("COMMAND_CALLBACK", method, JSON.stringify(params));

  if (!CommandCallbacks[method]) {
    console.debugLog(`Missing command callback: ${method}`);
    return {};
  }

  return CommandCallbacks[method](params);
}

function Target_getCurrentMessageContents() {
  return {};
}

module.exports = {
  initializeRecordReplay,
};
