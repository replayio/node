'use strict';

// Methods for interacting with the record/replay driver.

function initializeRecordReplay() {
  process.recordReplaySetCommandCallback(commandCallback);
  console.debugLog("HELLO");
}

function commandCallback(method, params) {
  console.debugLog("COMMAND_CALLBACK", method);
}

module.exports = {
  initializeRecordReplay,
};
