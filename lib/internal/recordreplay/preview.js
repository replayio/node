// Logic for creating object previews for the record/replay protocol.

const {
  protocolIdToRemoteObject,
  remoteObjectToProtocolId,
  remoteObjectToProtocolValue,
  scopeToProtocolId,
  protocolIdToScope,
} = require("internal/recordreplay/object");
const { sendMessage } = require("internal/recordreplay/message");
const { log } = require("internal/recordreplay/utils");

function createProtocolObject(objectId, level) {
  const obj = protocolIdToRemoteObject(objectId);
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

  addProperty(property) {
    if (!this.canAddItem()) {
      return;
    }
    if (!this.properties) {
      this.properties = [];
    }
    this.properties.push(property);
  },

  fill() {
    const properties = sendMessage("Runtime.getProperties", {
      objectId: this.obj.objectId,
      ownProperties: true,
      generatePreview: false,
    }).result;

    let prototype;
    for (const prop of properties) {
      if (prop.name == "__proto__") {
        prototype = prop;
      } else {
        const protocolProperty = createProtocolPropertyDescriptor(prop);
        this.addProperty(protocolProperty);
        if (this.overflow) {
          break;
        }
      }
    }

    let prototypeId;
    if (prototype && prototype.value && prototype.value.objectId) {
      prototypeId = remoteObjectToProtocolId(prototype.value);
    }

    return {
      prototypeId,
      overflow: this.overflow ? true : undefined,
      properties: this.properties,
    };
  },
};

function createProtocolPropertyDescriptor(desc) {
  const { name, value, writable, get, set, configurable, enumerable } = desc;

  const rv = value ? remoteObjectToProtocolValue(value) : {};
  rv.name = name;

  let flags = 0;
  if (writable) {
    flags |= 1;
  }
  if (configurable) {
    flags |= 2;
  }
  if (enumerable) {
    flags |= 4;
  }
  if (flags != 7) {
    rv.flags = flags;
  }

  if (get && get.objectId) {
    rv.get = remoteObjectToProtocolId(get);
  }
  if (set && set.objectId) {
    rv.set = remoteObjectToProtocolId(set);
  }

  return rv;
}

function createProtocolLocation(location) {
  if (!location) {
    return undefined;
  }
  const { scriptId, lineNumber, columnNumber } = location;
  return [{
    sourceId: scriptId,
    // CDP line numbers are 0-indexed, while RRP line numbers are 1-indexed.
    line: lineNumber + 1,
    column: columnNumber,
  }];
}

function createProtocolFrame(frameId, frame) {
  // CDP call frames don't provide detailed type information.
  const type = frame.functionName ? "call" : "global";

  return {
    frameId,
    type,
    functionName: frame.functionName || undefined,
    functionLocation: createProtocolLocation(frame.functionLocation),
    location: createProtocolLocation(frame.location),
    scopeChain: frame.scopeChain.map(scopeToProtocolId),
    this: remoteObjectToProtocolValue(frame.this),
  };
}

function createProtocolScope(scopeId) {
  const scope = protocolIdToScope(scopeId);

  let type;
  switch (scope.type) {
    case "global":
      type = "global";
      break;
    case "with":
      type = "with";
      break;
    default:
      type = scope.name ? "function" : "block";
      break;
  }

  let object, bindings;
  if (type == "global" || type == "with") {
    object = remoteObjectToProtocolId(scope.object);
  } else {
    bindings = [];

    const properties = sendMessage("Runtime.getProperties", {
      objectId: scope.object.objectId,
      ownProperties: true,
      generatePreview: false,
    }).result;
    for (const { name, value } of properties) {
      const converted = remoteObjectToProtocolValue(value);
      bindings.push({ ...converted, name });
    }
  }

  return {
    scopeId,
    type,
    object,
    functionName: scope.name || undefined,
    bindings,
  };
}

module.exports = {
  createProtocolObject,
  createProtocolFrame,
  createProtocolLocation,
  createProtocolScope,
};
