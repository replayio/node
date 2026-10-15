// Manage association between remote objects and protocol object IDs.

const { assert, log } = require("internal/recordreplay/utils");
const { sendMessage } = require("internal/recordreplay/message");

// Object group of the objects wrapped for pauses, released when the pause data
// is cleared (the native makeDebuggeeValue binding uses the same name).
const REPLAY_CDT_PAUSE_OBJECT_GROUP = "REPLAY_CDT_PAUSE_OBJECT_GROUP";

// Map protocol ObjectId => RemoteObject
const gProtocolIdToObject = new Map();

// Map RemoteObject.objectId => protocol ObjectId
const gObjectIdToProtocolId = new Map();

// Map object => protocol ObjectId
const gProtocolIdByPlainObject = new Map();

// Map protocol ObjectId => object
const gPlainObjectByProtocolId = new Map();

// Map protocol ScopeId => Debugger.Scope
const gProtocolIdToScope = new Map();

let gNextObjectId = 1;

function clearPauseDataCallback() {
  try {
    gProtocolIdToObject.clear();
    gObjectIdToProtocolId.clear();
    gProtocolIdByPlainObject.clear();
    gPlainObjectByProtocolId.clear();
    gProtocolIdToScope.clear();
    gNextObjectId = 1;

    sendMessage("Runtime.releaseObjectGroup", {
      objectGroup: REPLAY_CDT_PAUSE_OBJECT_GROUP,
    });
  } catch (e) {
    log(`JS clearPauseDataCallback exception: ${e}`);
  }
}

/**
 * Creates and returns a new CDP RemoteObject for the given value.
 */
function makeDebuggeeValue(plainValue) {
  return process.recordreplay._makeDebuggeeValue(plainValue);
}

function createRrpValueRaw(plainValue) {
  const cdpObject = makeDebuggeeValue(plainValue);
  return remoteObjectToProtocolValue(cdpObject);
}

function getPlainObjectByCdpId(cdpId) {
  const protocolObjectId = gObjectIdToProtocolId.get(cdpId);
  assert(protocolObjectId);
  return getPlainObjectByProtocolId(protocolObjectId);
}

function getPlainObjectByProtocolId(protocolObjectId) {
  let plainObject = gPlainObjectByProtocolId.get(protocolObjectId);
  if (!plainObject) {
    const remoteObject = protocolIdToRemoteObject(protocolObjectId);
    plainObject = process.recordreplay._getObjectByCdpId(remoteObject.objectId);
    gProtocolIdByPlainObject.set(plainObject, protocolObjectId);
    gPlainObjectByProtocolId.set(protocolObjectId, plainObject);
  }
  return plainObject;
}

const cdpRefTypes = ["object", "function"];
function isCdpRefType(remoteObject) {
  return cdpRefTypes.includes(remoteObject.type);
}

function remoteObjectToProtocolId(remoteObject) {
  assert(remoteObject.objectId);

  const existing = gObjectIdToProtocolId.get(remoteObject.objectId);
  if (existing) {
    return existing;
  }

  let protocolObjectId;
  let plainObject;
  if (isCdpRefType(remoteObject)) {
    // NOTE: the same object might generate multiple CDP ids
    plainObject = process.recordreplay._getObjectByCdpId(remoteObject.objectId);
    if (plainObject) {
      protocolObjectId = gProtocolIdByPlainObject.get(plainObject);
    }
  }

  const existingProtocolObjectId = protocolObjectId;
  protocolObjectId ||= (gNextObjectId++).toString();
  gObjectIdToProtocolId.set(remoteObject.objectId, protocolObjectId);
  gProtocolIdToObject.set(protocolObjectId, remoteObject);
  if (plainObject && !existingProtocolObjectId) {
    gProtocolIdByPlainObject.set(plainObject, protocolObjectId);
    gPlainObjectByProtocolId.set(protocolObjectId, plainObject);
  }

  return protocolObjectId;
}

function protocolIdToRemoteObject(objectId) {
  const remoteObject = gProtocolIdToObject.get(objectId);
  assert(remoteObject);
  return remoteObject;
}

// Strings longer than this will be truncated when creating protocol values.
const MaxStringLength = 10000;

function remoteObjectToProtocolValue(obj) {
  if (!obj) {
    return {};
  }
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
      if (typeof obj.value == "string" && obj.value.length > MaxStringLength) {
        return { value: obj.value.substring(0, MaxStringLength) + "…" };
      }
      return { value: obj.value };
    case "bigint": {
      const str = obj.unserializableValue;
      assert(str);
      return { bigint: str.substring(0, str.length - 1) };
    }
    case "object":
    case "function": {
      if (!obj.objectId) {
        return { value: null };
      }
      const object = remoteObjectToProtocolId(obj);
      return { object };
    }
    case "symbol":
      return { symbol: obj.description };
    default:
      log(`[RuntimeError] invalid CDP type: ${JSON.stringify(obj)}`);
      return { unavailable: true };
  }
}

function scopeToProtocolId(scope) {
  // Use the scope object's ID as the ID for the scope itself.
  const id = remoteObjectToProtocolId(scope.object);
  gProtocolIdToScope.set(id, scope);
  return id;
}

function protocolIdToScope(scopeId) {
  const scope = gProtocolIdToScope.get(scopeId);
  assert(scope);
  return scope;
}

module.exports = {
  REPLAY_CDT_PAUSE_OBJECT_GROUP,
  getPlainObjectByCdpId,
  createRrpValueRaw,
  remoteObjectToProtocolId,
  protocolIdToRemoteObject,
  remoteObjectToProtocolValue,
  scopeToProtocolId,
  protocolIdToScope,
  clearPauseDataCallback,
};
