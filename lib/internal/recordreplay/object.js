// Manage association between remote objects and protocol object IDs.

const { assert, log } = require("internal/recordreplay/utils");

// Map protocol ObjectId => RemoteObject
const gProtocolIdToObject = new Map();

// Map RemoteObject.objectId => protocol ObjectId
const gObjectIdToProtocolId = new Map();

// Map protocol ScopeId => Debugger.Scope
const gProtocolIdToScope = new Map();

let gNextObjectId = 1;

function clearPauseDataCallback() {
  gProtocolIdToObject.clear();
  gObjectIdToProtocolId.clear();
  gProtocolIdToScope.clear();
  gNextObjectId = 1;
}

function remoteObjectToProtocolId(remoteObject) {
  assert(remoteObject.objectId);

  const existing = gObjectIdToProtocolId.get(remoteObject.objectId);
  if (existing) {
    return existing;
  }

  const protocolObjectId = (gNextObjectId++).toString();
  gObjectIdToProtocolId.set(remoteObject.objectId, protocolObjectId);
  gProtocolIdToObject.set(protocolObjectId, remoteObject);

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
  remoteObjectToProtocolId,
  protocolIdToRemoteObject,
  remoteObjectToProtocolValue,
  scopeToProtocolId,
  protocolIdToScope,
  clearPauseDataCallback,
};
