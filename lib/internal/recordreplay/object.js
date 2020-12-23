// Manage association between remote objects and protocol object IDs.

const { assert } = require("internal/recordreplay/utils");

// Map protocol ObjectId => RemoteObject
const gProtocolIdToObject = new Map();

// Map RemoteObject.objectId => protocol ObjectId
const gObjectIdToProtocolId = new Map();

let gNextObjectId = 1;

function getRemoteObjectProtocolId(remoteObject) {
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
    case "function": {
      const object = getRemoteObjectProtocolId(obj);
      return { object };
    }
    default:
      return { unavailable: true };
  }
}

function getRemoteObject(objectId) {
  const remoteObject = gProtocolIdToObject.get(objectId);
  assert(remoteObject);
  return remoteObject;
}

module.exports = {
  remoteObjectToProtocolValue,
  getRemoteObject,
};
