// Interface for sending/receiving CDP messages.

const { log, assert } = require("internal/recordreplay/utils");

function initMessages() {
  process._recordReplaySetCDPMessageCallback(messageCallback);
}

let gNextMessageId = 1;

const gMessageCallbacks = new Map();

function sendMessage(method, params, onFinished) {
  const id = gNextMessageId++;
  gMessageCallbacks.set(id, onFinished);
  process._recordReplaySendCDPMessage(JSON.stringify({ method, params, id }));
}

const gEventListeners = new Map();

function addEventListener(method, callback) {
  gEventListeners.set(method, callback);
}

function messageCallback(message) {
  log(`MESSAGE_CALLBACK ${message}`);
  try {
    message = JSON.parse(message);

    if (message.id) {
      const onFinished = gMessageCallbacks.get(message.id);
      gMessageCallbacks.delete(message.id);
      if (onFinished) {
          onFinished(message.result);
      }
    } else {
      const listener = gEventListeners.get(message.method);
      if (listener) {
        listener(message.params);
      }
    }
  } catch (e) {
    log(`Message callback exception: ${e}`);
  }
}

function getRemoteObjectProperties(remoteObjectId) {
  let properties;
  sendMessage(
    "Runtime.getProperties",
    { objectId: remoteObjectId, ownProperties: true, generatePreview: false },
    ({ result }) => {
      properties = result;
    }
  );
  assert(properties);
  return properties;
}

module.exports = {
  initMessages,
  sendMessage,
  addEventListener,
  getRemoteObjectProperties,
};
