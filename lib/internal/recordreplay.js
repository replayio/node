'use strict';

// Methods for interacting with the record/replay driver.

function assert(v) {
  if (!v) {
    console.debugLog(`Assertion failed ${Error().stack}`);
    throw new Error("Assertion failed");
  }
}

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
  try {
    message = JSON.parse(message);

    if (message.id) {
      const onFinished = gMessageCallbacks.get(message.id);
      gMessageCallbacks.delete(message.id);
      if (onFinished) {
          onFinished(message.result);
      }
      return;
    }

    switch (message.method) {
      case "Runtime.consoleAPICalled":
        gLastConsoleAPICall = message.params;
      default:
        break;
    }
  } catch (e) {
    console.debugLog(`Message callback exception: ${e}`);
  }
}

const CommandCallbacks = {
  "Target.getCurrentMessageContents": Target_getCurrentMessageContents,
  "Pause.getObjectPreview": Pause_getObjectPreview,
};

function commandCallback(method, params) {
  console.debugLog("COMMAND_CALLBACK", method, JSON.stringify(params));

  if (!CommandCallbacks[method]) {
    console.debugLog(`Missing command callback: ${method}`);
    return {};
  }

  try {
    return CommandCallbacks[method](params);
  } catch (e) {
    console.debugLog(`Error: Command exception ${e}`);
    return {};
  }
}

// Contents of the last console API call. Runtime.consoleAPICalled will be
// emitted before the driver gets the current message contents.
let gLastConsoleAPICall;

function Target_getCurrentMessageContents() {
  // The message arguments are stored directly on `process` before calling
  // recordReplayOnConsoleAPI. Extract information about them here from the
  // CDP. The arguments are also stored on the last console API call, though
  // if we use that we need to be careful because the pause state could have
  // been cleared since the last Runtime.consoleAPICalled event.
  let argumentsId;
  assert(process.recordReplayConsoleArgs);
  sendMessage(
    "Runtime.evaluate",
    { expression: "process.recordReplayConsoleArgs" },
    ({ result }) => {
      assert(result.type == "object");
      assert(result.className == "Array");
      argumentsId = result.objectId;
    }
  );
  assert(argumentsId);

  // Get the properties of the message arguments array.
  let argumentsProperties;
  sendMessage(
    "Runtime.getProperties",
    { objectId: argumentsId, ownProperties: true, generatePreview: false },
    ({ result }) => {
      argumentsProperties = result;
    }
  );
  assert(argumentsProperties);

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

// Map ObjectId => RemoteObject
const gObjects = new Map();

function remoteObjectToProtocolValue(obj) {
  switch (obj.type) {
    case "undefined":
      return {};
    case "string":
    case "number":
    case "boolean":
      if (obj.unserializableValue) {
        assert(obj.type == "number");
        return { unserializableNumber: obj.unserializableValue };
      }
      return { value: obj.value };
    case "bigint":
      assert(obj.unserializableValue);
      return { bigint: obj.unserializableValue };
    case "object":
    case "function":
      assert(obj.objectId);
      gObjects.set(obj.objectId, obj);
      return { object: obj.objectId };
    default:
      return { unavailable: true };
  }
}

function Pause_getObjectPreview({ object, level = "full" }) {
  const objectData = createProtocolObject(object, level);
  return { data: { objects: [objectData] } };
}

function createProtocolObject(objectId, level) {
  const obj = gObjects.get(objectId);
  if (!obj) {
    console.debugLog("Error: createProtocolObject unknown object");
    return { objectId, className: "BadObjectId" };
  }

  const className = obj.className || "Function";

  let preview;
  if (level != "none") {
    preview = new ProtocolObjectPreview(obj, level).fill();
  }

  return { objectId, className, preview };
}

const NumItemsBeforeOverflow = 10;

function ProtocolObjectPreview(obj, level) {
  this.obj = obj;
  this.level = level;
  this.overflow = false;
  this.numItems = 0;
}

ProtocolObjectPreview.prototype = {
  canAddItem(force) {
    if (this.level == "noProperties") {
      this.overflow = true;
      return false;
    }
    if (!force && this.level == "canOverflow" && this.numItems >= NumItemsBeforeOverflow) {
      this.overflow = true;
      return false;
    }
    this.numItems++;
    return true;
  },

  fill() {
  },
};

module.exports = {
  initializeRecordReplay,
};
