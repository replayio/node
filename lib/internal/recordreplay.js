'use strict';

// Methods for interacting with the record/replay driver.

function initializeRecordReplay() {
  console.debugLog("INITIALIZE_RECORD_REPLAY");
  if (process.isRecordingOrReplaying()) {
    process._recordReplaySetCommandCallback(commandCallback);
    process._recordReplaySetCDPMessageCallback(messageCallback);
    sendMessage("Runtime.enable");
  }
}

let gNextMessageId = 1;

const gMessageCallbacks = new Map();

function sendMessage(method, params, onFinished) {
  const id = gNextMessageId++;
  gMessageCallbacks.set(id, onFinished);
  process._recordReplaySendCDPMessage(JSON.stringify({ method, params, id }));
}

function messageCallback(message) {
  console.debugLog(`MESSAGE_CALLBACK ${message}`);
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
