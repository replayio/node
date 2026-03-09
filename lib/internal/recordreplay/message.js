// Interface for sending/receiving CDP messages.

const { log, assert } = require("internal/recordreplay/utils");

function initMessages() {
  process.recordreplay._setCDPMessageCallback(messageCallback);
}

let gNextMessageId = 1;

let gCurrentMessageId;
let gCurrentMessageResult;

function sendMessage(method, params) {
  const id = gNextMessageId++;
  gCurrentMessageId = id;
  process.recordreplay._sendCDPMessage(JSON.stringify({ method, params, id }));
  gCurrentMessageId = undefined;
  return gCurrentMessageResult;
}

const gEventListeners = new Map();

function addEventListener(method, callback) {
  gEventListeners.set(method, callback);
}

function messageCallback(message) {
  try {
    message = JSON.parse(message);

    if (message.id) {
      assert(message.id == gCurrentMessageId);
      gCurrentMessageResult = message.result;
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

module.exports = {
  initMessages,
  sendMessage,
  addEventListener,
};
