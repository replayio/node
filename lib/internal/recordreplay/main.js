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
  protocolIdToRemoteObject,
} = require("internal/recordreplay/object");
const {
  createProtocolObject,
  createProtocolFrame,
  createProtocolLocation,
  createProtocolScope,
} = require("internal/recordreplay/preview");
const {
  registerSourceMap,
  processSourceMaps,
  getSourceMapURL
} = require("internal/recordreplay/sourcemap");
const { assert, log } = require("internal/recordreplay/utils");

function initializeRecordReplay() {
  if (process.recordreplay) {
    initMessages();
    process.recordreplay._setCommandCallback(commandCallback);
    process.recordreplay._setClearPauseDataCallback(clearPauseDataCallback);
    addEventListener("Runtime.consoleAPICalled", onConsoleAPICall);
    addEventListener("Debugger.scriptParsed", registerSourceMap);
    process.on("exit", processSourceMaps);
    sendMessage("Runtime.enable");
    sendMessage("Debugger.enable");
  }
}

const CommandCallbacks = {
  "Target.countStackFrames": Target_countStackFrames,
  "Target.evaluatePrivileged": Target_evaluatePrivileged,
  "Target.getCurrentMessageContents": Target_getCurrentMessageContents,
  "Target.getSourceMapURL": Target_getSourceMapURL,
  "Target.getStepOffsets": Target_getStepOffsets,
  "Target.topFrameLocation": Target_topFrameLocation,
  "Pause.evaluateInFrame": Pause_evaluateInFrame,
  "Pause.evaluateInGlobal": Pause_evaluateInGlobal,
  "Pause.getAllFrames": Pause_getAllFrames,
  "Pause.getExceptionValue": Pause_getExceptionValue,
  "Pause.getObjectPreview": Pause_getObjectPreview,
  "Pause.getObjectProperty": Pause_getObjectProperty,
  "Pause.getScope": Pause_getScope,
};

function commandCallback(method, params) {
  if (!CommandCallbacks[method]) {
    log(`Missing command callback: ${method}`);
    return {};
  }

  try {
    return CommandCallbacks[method](params);
  } catch (e) {
    log(`Error: Command exception ${method} ${e}`);
    return {};
  }
}

function Target_countStackFrames() {
  const count = getStackFrames().length;
  return { count };
}

function Target_evaluatePrivileged({ expression }) {
  const result = eval(expression);
  return { result };
}

// Contents of the last console API call. Runtime.consoleAPICalled will be
// emitted before the driver gets the current message contents.
let gLastConsoleAPICall;

function onConsoleAPICall(params) {
  gLastConsoleAPICall = params;
}

function Target_getCurrentMessageContents() {
  // We could be getting the contents of either an error object that was reported
  // to the driver via C++, or a console API call that was reported to the driver
  // via onConsoleAPICall().
  const error = process.recordreplay._getCurrentError();

  if (error) {
    const { message, filename, line, column, scriptId } = error;
    return {
      source: "PageError",
      level: "error",
      text: message,
      url: filename,
      sourceId: scriptId ? scriptId.toString() : undefined,
      line,
      column,
    };
  }

  // Look for the "args" variable on an onConsoleMessage frame.
  // The arguments are also stored on the last console API call, though
  // if we use that we need to be careful because the pause state could have
  // been cleared since the last Runtime.consoleAPICalled event.
  const { callFrames } = sendMessage("Debugger.getCallFrames");
  const consoleMessageFrame = callFrames.find(
    frame => frame.functionName == "onConsoleMessage"
  );
  assert(consoleMessageFrame);
  assert(consoleMessageFrame.this.type == "object");
  assert(consoleMessageFrame.this.className == "Array");
  const argumentsId = consoleMessageFrame.this.objectId;

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

function Target_getSourceMapURL({ sourceId }) {
  const url = getSourceMapURL(sourceId);
  return { url, baseUrl: url };
}

function Target_getStepOffsets() {
  // CDP does not distinguish between steps and breakpoints.
  return {};
}

function Target_topFrameLocation() {
  // Instead of asking for the locations of all frames initially, see if we can
  // find a frame that isn't being ignored near the top, so we don't have to walk
  // the entire stack.
  const InitialMaxFrames = 10;
  const { frameLocations } = sendMessage("Debugger.getFrameLocations", { maxFrames: InitialMaxFrames });

  for (const location of frameLocations) {
    if (!process.recordreplay._ignoreScript(location.scriptId)) {
      return { location: createProtocolLocation(location)[0] };
    }
  }

  if (frameLocations.length == InitialMaxFrames) {
    const { frameLocations: allFrameLocations } = sendMessage("Debugger.getFrameLocations");
    for (const location of allFrameLocations) {
      if (!process.recordreplay._ignoreScript(location.scriptId)) {
        return { location: createProtocolLocation(location)[0] };
      }
    }
  }

  return {};
}

// Get the raw call frames on the stack, eliding ones in scripts we are ignoring.
function getStackFrames() {
  const { callFrames } = sendMessage("Debugger.getCallFrames");

  const frames = [];
  for (const frame of callFrames) {
    if (!process.recordreplay._ignoreScript(frame.location.scriptId)) {
      frames.push(frame);
    }
  }
  return frames;
}

// Build a protocol Result object from a result/exceptionDetails CDP rval.
function buildProtocolResult({ result, exceptionDetails }) {
  const value = remoteObjectToProtocolValue(result);
  const protocolResult = { data: {} };

  if (exceptionDetails) {
    protocolResult.exception = value;
  } else {
    protocolResult.returned = value;
  }
  return { result: protocolResult };
}

function Pause_evaluateInFrame({ frameId, expression }) {
  const frames = getStackFrames();
  const index = +frameId;
  assert(index < frames.length);
  const frame = frames[index];

  const rv = doEvaluation();
  return buildProtocolResult(rv);

  function doEvaluation() {
    // In order to do the evaluation in the right frame, the same number of
    // frames need to be on V8's stack when we do the evaluation as when we got
    // the stack frames in the first place. The debugger agent extracts a frame
    // index from the ID it is given and uses that to walk the stack to the
    // frame where it will do the evaluation (see DebugStackTraceIterator).
    return sendMessage(
      "Debugger.evaluateOnCallFrame",
      {
        callFrameId: frame.callFrameId,
        expression,
      }
    );
  }
}

function Pause_evaluateInGlobal({ expression }) {
  const rv = sendMessage("Runtime.evaluate", { expression });
  return buildProtocolResult(rv);
}

function Pause_getAllFrames() {
  const frames = getStackFrames().map((frame, index) => {
    // Use our own IDs for frames.
    const id = (index++).toString();
    return createProtocolFrame(id, frame);
  });

  return {
    frames: frames.map(f => f.frameId),
    data: { frames },
  };
}

function Pause_getExceptionValue() {
  const rv = sendMessage("Debugger.getPendingException", {});
  return { exception: remoteObjectToProtocolValue(rv.exception), data: {} };
}

function Pause_getObjectPreview({ object, level = "full" }) {
  const objectData = createProtocolObject(object, level);
  return { data: { objects: [objectData] } };
}

function Pause_getObjectProperty({ object, name }) {
  const obj = protocolIdToRemoteObject(object);
  const rv = sendMessage(
    "Runtime.callFunctionOn",
    {
      functionDeclaration: `function() { return this["${name}"] }`,
      objectId: obj.objectId,
    }
  );
  return buildProtocolResult(rv);
}

function Pause_getScope({ scope }) {
  const scopeData = createProtocolScope(scope);
  return { data: { scopes: [scopeData] } };
}

module.exports = {
  initializeRecordReplay,
};
