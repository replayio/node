'use strict';

// Methods for interacting with the record/replay driver.

const {
  initMessages,
  sendMessage,
  addEventListener,
} = require("internal/recordreplay/message");
const {
  remoteObjectToProtocolValue,
  clearPauseDataCallback,
} = require("internal/recordreplay/object");
const {
  createProtocolObject,
  createProtocolFrame,
} = require("internal/recordreplay/preview");
const { assert, log } = require("internal/recordreplay/utils");

function initializeRecordReplay() {
  if (process.isRecordingOrReplaying()) {
    initMessages();
    process._recordReplaySetCommandCallback(commandCallback);
    process._recordReplaySetClearPauseDataCallback(clearPauseDataCallback);
    addEventListener("Runtime.consoleAPICalled", onConsoleAPICall);
    sendMessage("Runtime.enable");
  }
}

const CommandCallbacks = {
  "Target.getCurrentMessageContents": Target_getCurrentMessageContents,
  "Target.getSourceMapURL": Target_getSourceMapURL,
  "Pause.getAllFrames": Pause_getAllFrames,
  "Pause.getObjectPreview": Pause_getObjectPreview,
};

function commandCallback(method, params) {
  log(`COMMAND_CALLBACK ${method} ${JSON.stringify(params)}`);

  if (!CommandCallbacks[method]) {
    log(`Missing command callback: ${method}`);
    return {};
  }

  try {
    return CommandCallbacks[method](params);
  } catch (e) {
    log(`Error: Command exception ${e}`);
    return {};
  }
}

// Contents of the last console API call. Runtime.consoleAPICalled will be
// emitted before the driver gets the current message contents.
let gLastConsoleAPICall;

function onConsoleAPICall(params) {
  gLastConsoleAPICall = params;
}

function Target_getCurrentMessageContents() {
  // The message arguments are stored directly on `process` before calling
  // recordReplayOnConsoleAPI. Extract information about them here from the
  // CDP. The arguments are also stored on the last console API call, though
  // if we use that we need to be careful because the pause state could have
  // been cleared since the last Runtime.consoleAPICalled event.
  assert(process.recordReplayConsoleArgs);
  const rv = sendMessage(
    "Runtime.evaluate",
    { expression: "process.recordReplayConsoleArgs" },
  );
  assert(rv.result.type == "object");
  assert(rv.result.className == "Array");
  const argumentsId = rv.result.objectId;

  // Get the properties of the message arguments array.
  const argumentsProperties = sendMessage("Runtime.getProperties", {
    objectId: argumentsId,
    ownProperties: true,
    generatePreview: false,
  }).result;

  // Get the protocol representation of the message arguments.
  const argumentValues = [];
  for (let i = 0;; i++) {
    const property = argumentsProperties.find(prop => prop.name == i.toString());
    if (!property) {
      break;
    }
    argumentValues.push(remoteObjectToProtocolValue(property.value));
  }

  let level = "info";
  switch (gLastConsoleAPICall.level) {
    case "warning":
      level = "warning";
      break;
    case "error":
      level = "error";
      break;
  }

  let url, sourceId, line, column;
  if (gLastConsoleAPICall.stackTrace) {
    const frame = gLastConsoleAPICall.stackTrace.callFrames[0];
    if (frame) {
      url = frame.url;
      sourceId = frame.scriptId;
      line = frame.lineNumber;
      column = frame.columnNumber;
    }
  }

  return {
    source: "ConsoleAPI",
    level,
    text: "",
    url,
    sourceId,
    line,
    column,
    argumentValues,
  };
}

function Target_getSourceMapURL() {
  // NYI
  return {};
}

function Pause_getAllFrames() {
  const { callFrames } = sendMessage("Debugger.getCallFrames");

  const frames = [];

  let sawCommandCallback = false;
  let index = 0;
  for (const frame of callFrames) {
    // Ignore all frame son the stack up to the commandCallback call.
    if (!sawCommandCallback) {
      if (frame.functionName == "commandCallback") {
        sawCommandCallback = true;
      }
      continue;
    }

    // Use our own IDs for frames.
    const id = (index++).toString();
    frames.push(createProtocolFrame(id, frame));
  }

  return {
    frames: frames.map(f => f.frameId),
    data: { frames },
  };
}

function Pause_getObjectPreview({ object, level = "full" }) {
  const objectData = createProtocolObject(object, level);
  return { data: { objects: [objectData] } };
}

module.exports = {
  initializeRecordReplay,
};
