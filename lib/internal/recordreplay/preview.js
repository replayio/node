// Logic for creating object previews for the record/replay protocol.

const {
  getRemoteObject,
  getRemoteObjectProtocolId,
  remoteObjectToProtocolValue,
} = require("internal/recordreplay/object");
const { getRemoteObjectProperties } = require("internal/recordreplay/message");
const { log } = require("internal/recordreplay/utils");

function createProtocolObject(objectId, level) {
  const obj = getRemoteObject(objectId);
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
    const properties = getRemoteObjectProperties(this.obj.objectId);

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
      prototypeId = getRemoteObjectProtocolId(prototype.value);
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

  if (get) {
    rv.get = getRemoteObjectProtocolId(get);
  }
  if (set) {
    rv.set = getRemoteObjectProtocolId(set);
  }

  return rv;
}

module.exports = {
  createProtocolObject,
};
