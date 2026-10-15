// Interface for sending/receiving CDP messages.

const { log, assert } = require("internal/recordreplay/utils");

function initMessages() {
  process.recordreplay._setCDPMessageCallback(messageCallback);
}

let gNextMessageId = 1;

let gCurrentMessageId;
let gCurrentMessageResult;
let gCurrentMessageError;

class CDPMessageError extends Error {
  constructor(message, code) {
    super(`${message} (${code})`);
    this.cdpMessage = message;
    this.code = code;
  }
}

function sendMessage(method, params) {
  const id = gNextMessageId++;
  gCurrentMessageId = id;
  gCurrentMessageResult = undefined;
  gCurrentMessageError = undefined;
  process.recordreplay._sendCDPMessage(JSON.stringify({ method, params, id }));
  gCurrentMessageId = undefined;
  if (gCurrentMessageError) {
    throw new CDPMessageError(gCurrentMessageError.message, gCurrentMessageError.code);
  }
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
      gCurrentMessageError = message.error;
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
  CDPMessageError,
  initMessages,
  sendMessage,
  addEventListener,
};
