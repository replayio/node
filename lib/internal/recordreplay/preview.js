// Logic for creating object previews for the record/replay protocol.

const { getRemoteObject } = require("internal/recordreplay/object");

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

  fill() {
  },
};

module.exports = {
  createProtocolObject,
};
