// Logic for creating object previews for the record/replay protocol.
// This follows Chromium's ProtocolObjectPreview in replay_command_handlers.js.

const {
  REPLAY_CDT_PAUSE_OBJECT_GROUP,
  protocolIdToRemoteObject,
  remoteObjectToProtocolId,
  remoteObjectToProtocolValue,
  scopeToProtocolId,
  protocolIdToScope,
  getPlainObjectByCdpId,
  createRrpValueRaw,
} = require("internal/recordreplay/object");
const { sendMessage, CDPMessageError } = require("internal/recordreplay/message");
const { log } = require("internal/recordreplay/utils");

function isCdpObjectProxy(cdpObj) {
  return cdpObj.subtype === "proxy";
}

function isCdpObjectPromise(cdpObj) {
  return cdpObj.subtype === "promise";
}

function createProtocolObject(objectId, level, pageSizeForTesting) {
  const obj = protocolIdToRemoteObject(objectId);
  const className = isCdpObjectProxy(obj) ? "Proxy" : (obj.className || "Function");

  let preview;
  if (level != "none") {
    preview = new ProtocolObjectPreview(objectId, obj, level, pageSizeForTesting).fill();
  }

  return { objectId, className, preview };
}

// Return whether an object should be ignored when generating previews.
function isObjectBlacklisted(cdpObj) {
  // Don't inspect scripted proxies, as we could end up calling into script.
  if (isCdpObjectProxy(cdpObj)) {
    return true;
  }

  return false;
}

// Return whether an object's property should be ignored when generating previews.
function isObjectPropertyBlacklisted(cdpObj, name) {
  if (isObjectBlacklisted(cdpObj)) {
    return true;
  }
  switch (name) {
    case "__proto__":
      // Accessing __proto__ doesn't cause problems, but is redundant with the
      // prototype reference included in the preview directly.
      return true;
  }
  return false;
}

// Target limit for the number of items (properties etc.) to include in object
// previews before overflowing.
const MaxItems = {
  "noProperties": 0,

  // Note: this is higher than on gecko-dev because typed arrays don't render
  // properly in the devtools currently unless we include a minimum number of
  // properties. This would be nice to fix.
  "canOverflow": 10,

  "full": 1000,
};

function ProtocolObjectPreview(rrpId, obj, level, pageSizeForTesting) {
  this.rrpId = rrpId;
  this.cdpObj = obj;
  this.level = level;
  this.pageSizeForTesting = pageSizeForTesting;
  this.overflow = false;
  this.numItems = 0;
  this.extra = {};
}

ProtocolObjectPreview.prototype = {
  get raw() {
    return this.plainObject;
  },

  get plainObject() {
    if (!this._plainObject) {
      this._plainObject = getPlainObjectByCdpId(this.cdpObj.objectId);
    }
    return this._plainObject;
  },

  startAddItem(force) {
    if (!force) {
      if (this.hasReachedItemLimit) {
        this.overflow = true;
        return false;
      }
      this.numItems++;
    }
    return true;
  },

  checkAddProperty(ownerCdpObject, name) {
    if (isObjectPropertyBlacklisted(ownerCdpObject, name)) {
      return false;
    }
    if (this.getterValues?.has(name)) {
      return false;
    }
    return true;
  },

  addPropertyUnchecked(rrpProp, force) {
    if (!this.startAddItem(force)) {
      return;
    }
    if (!this.properties) {
      this.properties = [];
    }
    this.properties.push(rrpProp);
  },

  addGetterValue(propKey, ownerCdpObject, force = false) {
    if (isObjectPropertyBlacklisted(ownerCdpObject, propKey)) {
      return;
    }

    if (!this.getterValues) {
      this.getterValues = new Map();
    }
    if (this.getterValues.has(propKey)) {
      return;
    }

    const rrpValue = evalPropRrpNotNull(this.raw, propKey);
    if (rrpValue) {
      this.setGetterValueUnchecked(propKey, rrpValue, force);
    }
  },

  setGetterValueUnchecked(key, valueObject, force = true) {
    if (!this.startAddItem(force)) {
      return;
    }
    if (!this.getterValues) {
      this.getterValues = new Map();
    }
    this.getterValues.set(key, { name: key, ...valueObject });
  },

  addContainerEntry(entry) {
    if (!this.startAddItem()) {
      return;
    }
    if (!this.containerEntries) {
      this.containerEntries = [];
    }
    this.containerEntries.push(entry);
  },

  get nRequestedItems() {
    return MaxItems[this.level] || 10;
  },

  get hasReachedItemLimit() {
    return this.numItems >= this.nRequestedItems;
  },

  /**
   * Limit the amount of props we get back from CDP Runtime.getProperties.
   */
  get pageSize() {
    if (this.pageSizeForTesting) {
      return this.pageSizeForTesting;
    }

    return this.nRequestedItems;
  },

  shouldAddProp(cdpProp) {
    return this.checkAddProperty(this.cdpObj, cdpProp.name);
  },

  fill() {
    // Data returned from V8 debugger.
    let cdpProperties;

    if (this.pageSizeForTesting && !(this.pageSizeForTesting > 0)) {
      throw new Error("invalid pageSizeForTesting: " + this.pageSizeForTesting);
    }

    // Names of properties + getters.
    const foundProps = new Set();

    if (this.level === "noProperties") {
      cdpProperties = { result: [] };
    } else {
      const propertiesToFetch = this.pageSize;

      try {
        cdpProperties = sendMessage("Runtime.getProperties", {
          objectId: this.cdpObj.objectId,
          ownProperties: true,
          generatePreview: false,
          pageIndex: 0, // Warning: NYI
          pageSize: propertiesToFetch + 1, // +1 so we can detect overflow
          objectGroup: REPLAY_CDT_PAUSE_OBJECT_GROUP,
        });
      } catch (e) {
        // Preview is best-effort: any CDP failure → empty props (match Chrome expand).
        if (e instanceof CDPMessageError) {
          log(
            `[crash-0050] ProtocolObjectPreview.fill CDP error ${e.code}: ${e.cdpMessage || e.message}`,
          );
          cdpProperties = { result: [] };
        } else {
          throw e;
        }
      }

      if (!cdpProperties.result) {
        return {
          prototypeId: undefined
        };
      }

      /**
       * @see https://chromedevtools.github.io/devtools-protocol/tot/Runtime/#type-PropertyDescriptor
       */
      for (let i = 0; i < cdpProperties.result.length; ++i) {
        const cdpProp = cdpProperties.result[i];
        const { name: propKey } = cdpProp;
        if (propKey === "__proto__" || foundProps.has(propKey)) {
          continue;
        }
        if (this.shouldAddProp(cdpProp)) {
          foundProps.add(propKey);
        }
      }
    }

    for (const cdpProp of cdpProperties.result) {
      const { name: propKey } = cdpProp;
      if (!foundProps.has(propKey)) {
        continue;
      }
      const rrpProp = createProtocolPropertyDescriptor(cdpProp);
      const force = false;
      this.addPropertyUnchecked(rrpProp, force);
    }

    // Add builtin-specific data, except for prototypes, which have the class
    // name but not the data.
    const previewers = CustomPreviewers[this.cdpObj.className];
    if (previewers && !isPrototype(this.raw)) {
      for (const entry of previewers) {
        if (typeof entry == "function") {
          entry.call(this, cdpProperties);
        } else {
          // entry should be string -> Look it up in results
          const cdpEntry = cdpProperties.result.find(prop => prop.name === entry);
          if (cdpEntry) {
            const rrpEntry = remoteObjectToProtocolValue(cdpEntry.value);
            this.setGetterValueUnchecked(entry, rrpEntry);
          }
        }
      }
    }
    // Add data for special objects.
    Object.assign(this.extra, getExtraObjectPreviewData(this.cdpObj, cdpProperties));
    // Add Prototype data.
    const prototypeCdp = getInternalProp(cdpProperties, "[[Prototype]]")?.value;
    let prototypeRrpId;
    if (prototypeCdp) {
      prototypeRrpId = remoteObjectToProtocolId(prototypeCdp);
    }

    // Produce final PauseData object.
    return {
      prototypeId: prototypeRrpId,
      overflow: (this.overflow && this.level != "full") ? true : undefined,
      properties: this.properties,
      getterValues: this.getterValues ? [...this.getterValues.values()] : undefined,
      containerEntries: this.containerEntries,
      ...this.extra,
    };
  },
};

function isPrototype(x) {
  // Note: This can invoke getters or proxy hooks on page objects,
  // so we watch for exceptions being thrown.
  try {
    return x === x?.constructor?.prototype;
  } catch (e) {
    return false;
  }
}

function getExtraObjectPreviewData(cdpObject, cdpProperties) {
  if (isCdpObjectProxy(cdpObject)) {
    const targetCdpObj = getInternalProp(cdpProperties, "[[Target]]")?.value;
    const handlerCdpObj = getInternalProp(cdpProperties, "[[Handler]]")?.value;
    return {
      proxyState: {
        target: remoteObjectToProtocolValue(targetCdpObj),
        handler: remoteObjectToProtocolValue(handlerCdpObj),
      },
    };
  } else if (isCdpObjectPromise(cdpObject)) {
    const stateCdpObj = getInternalProp(cdpProperties, "[[PromiseState]]")?.value;
    const valueCdpObj = getInternalProp(cdpProperties, "[[PromiseResult]]")?.value;
    const promiseState = {
      state: stateCdpObj.value || undefined,
    };
    if (promiseState.state !== "pending") {
      promiseState.value = remoteObjectToProtocolValue(valueCdpObj);
    }
    return {
      promiseState,
    };
  }
}

/**
 * Get given prop from given object and get its value.
 * Return RRP wrapper.
 * Since we only use this for a small set of well defined
 * props, we emit a warning if the result is undefined or null.
 */
function evalPropRrpNotNull(owner, propKey) {
  try {
    const plainValue = owner[propKey];
    if (plainValue === undefined || plainValue === null) {
      // [RUN-2223] This should not happen.
      const e = new Error("");
      log(`[RUN-2223] JS evalPropRrpNotNull got ${plainValue} when evaluating ${propKey} on ${typeof owner}, stack=${e.stack}`);
    }
    return createRrpValueRaw(plainValue);
  } catch (err) {
    log(`JS evalPropRrpNotNull exception - calling ${propKey?.toString?.()} on ${typeof owner} - ${err?.stack || err}`);
    return null;
  }
}

function getDescriptionCount(description) {
  const match = /\((\d+)\)/.exec(description || "");
  if (match) {
    return +match[1];
  }
  return undefined;
}

function previewArray(_cdpProperties) {
  const length = getDescriptionCount(this.cdpObj.description);
  this.setGetterValueUnchecked("length", createRrpValueRaw(length));
}

function previewTypedArray() {
  // simply invoke the native getter
  this.addGetterValue("length", this.cdpObj, /* force */ true);
  this.addGetterValue("byteLength", this.cdpObj, /* force */ true);
  this.addGetterValue("byteOffset", this.cdpObj, /* force */ true);
  this.addGetterValue("buffer", this.cdpObj, /* force */ true);
}

/**
 * Query the internal object of {Weak,}{Set,Map}s that store their
 * containerEntries.
 */
function previewSetMap(cdpProperties) {
  if (!cdpProperties.internalProperties) {
    return;
  }

  const internal = getInternalProp(cdpProperties, "[[Entries]]");
  if (!internal || !internal.value || !internal.value.objectId) {
    return;
  }

  // Get size from description.
  let size;

  if (["Set", "Map"].includes(this.cdpObj.className)) {
    // NOTE: The internal backing array size is capped to pageSize for Set and
    // Map, so query the size getter instead.
    // `raw` is null if the cdpId lookup fails.
    size = this.raw ? this.raw.size : 0;
    const rrpSize = { name: "size", value: size };
    this.addPropertyUnchecked(rrpSize, /* force */ true);
    this.setGetterValueUnchecked(rrpSize.name, rrpSize, /* force */ true);
  } else {
    // Weak{Set,Map}
    size = getDescriptionCount(internal.value.description);
  }
  this.extra.containerEntryCount = size;

  const entries = sendMessage("Runtime.getProperties", {
    objectId: internal.value.objectId,
    ownProperties: true,
    generatePreview: false,
    pageIndex: this.pageIndex,
    pageSize: this.pageSize,
    objectGroup: REPLAY_CDT_PAUSE_OBJECT_GROUP,
  }).result;

  for (const entry of entries) {
    if (entry?.value?.subtype == "internal#entry") {
      const entryProperties = sendMessage("Runtime.getProperties", {
        objectId: entry.value.objectId,
        ownProperties: true,
        generatePreview: false,
        objectGroup: REPLAY_CDT_PAUSE_OBJECT_GROUP,
      }).result;
      const key = entryProperties.find(eprop => eprop.name == "key");
      const value = entryProperties.find(eprop => eprop.name == "value");
      if (value) {
        this.addContainerEntry({
          key: key ? remoteObjectToProtocolValue(key.value) : undefined,
          value: remoteObjectToProtocolValue(value.value),
        });
      }
    }
    if (this.overflow) {
      break;
    }
  }
}

function previewRegExp() {
  this.extra.regexpString = this.cdpObj.description;
}

function previewDate() {
  const dateTime = Date.parse(this.cdpObj.description);
  if (!Number.isNaN(dateTime)) {
    this.extra.dateTime = dateTime;
  }
}

function previewError() {
  this.setGetterValueUnchecked("name", { value: this.cdpObj.className });
}

const ErrorProperties = [
  "message",
  "stack",
  previewError,
];

function getInternalProp(cdpProperties, name) {
  return cdpProperties.internalProperties?.find(
    prop => prop.name == name
  );
}

function getInternalFunctionLocationProp(cdpProperties) {
  return getInternalProp(cdpProperties, "[[FunctionLocation]]");
}

/**
 * String utility for function parameter parsing.
 */
function substringEndsAt(haystack, needle, i) {
  return haystack.substring(1 + i - needle.length, 1 + i) === needle;
}

/**
 * @return {boolean} (a + b).endsWith(end)
 */
function stringsEndWith(a, b, end) {
  // NOTE: This can be done more performantly.
  return (a + b).endsWith(end);
}

const complementaryTokensStart = "({[\"'`";
const complementaryTokensEnd =   ")}]\"'`";

/**
 * [RUN-3146] Extract parameter names from the description.
 * @param {string} s
 * @return {string[]}
 */
function extractFunctionParameterNames(s) {
  try {
    /**
     * Function head declaration patterns:
     * P1. modifiers function f(PARAMS) Body
     * P2. modifiers f(PARAMS) Body  // ObjectMethod, ClassMethod
     * P3. modifiers (PARAMS) => ExpressionOrBody
     * P4. modifiers PARAM => ExpressionOrBody
     */

    // All patterns fall into two buckets:
    // A. PARENS: Get the thing within the first opening pair of parentheses.
    // B. SINGLEARROW: Get the single param before =>.

    // Base case: Find pattern A or B, while ignoring comments.
    // Special case 1: Parameter initializers (e.g. `x = f()`).
    //    Commas can be part of initializers, but they would have to be contained
    //    by complementary tokens, any of: ``""''[](){}.
    //    → Let's simply ignore everything inside of that.
    // Special case 2: Argument destructuring.
    //    -> Don't give the param a name (empty string).
    //    Same parsing logic as case 1.

    /**
     * The function header without (i) comments and without (ii) nested AST nodes.
     */
    let cleanHeader = "";
    /**
     * When in a comment, this is set to the counterpart that we are looking for.
     * @type {string | null}
     */
    let expectedCommentEnd = null;
    let parensStart = -1;
    let insideInitializer = false;
    /**
     * @type {string[]}
     */
    const ignoreStack = [];
    for (let i = 0; i < s.length; ++i) {
      const c = s[i];
      if (expectedCommentEnd) {
        // In a comment.
        if (substringEndsAt(s, expectedCommentEnd, i)) {
          // Comment End: Start new segment from here.
          expectedCommentEnd = null;
        }
        continue;
      }

      // Not in a comment.
      if (cleanHeader.endsWith("/") && (c === "/" || c === "*")) {
        // Comment Start.
        if (c === "*") {
          expectedCommentEnd = "*/";
        } else {
          expectedCommentEnd = "\n";
        }
        // Remove "/" from header:
        cleanHeader = cleanHeader.slice(0, -1);
        continue;
      }

      // Parse everything but comments:

      if (parensStart === -1) {
        // Not in params parentheses.
        if (c === "(") {
          // PARENS: Params start.
          cleanHeader += c;
          parensStart = cleanHeader.length;
          continue;
        }

        if (stringsEndWith(cleanHeader, c, "=>")) {
          // SINGLEARROW: Found the arrow → The last word in the header (sans "=") is the param.
          const param = cleanHeader.trim().match(/([^\s]+)\s*=$/)?.[1];
          return param ? [param] : [];
        }

        // otherwise keep the character
        cleanHeader += c;
        continue;
      }

      // destructuring
      if (complementaryTokensStart.includes(c)) {
        // Inside destructuring argument or initializer expression:
        // Enter node. Push complementary (to be searched for) onto stack.
        const tokenIdx = complementaryTokensStart.indexOf(c);
        ignoreStack.push(complementaryTokensEnd[tokenIdx]);
        continue; // don't include destructuring in the header
      }
      if (ignoreStack.length) {
        // Inside destructuring or initializer expression.
        if (c === ignoreStack[ignoreStack.length - 1]) {
          // Exit node.
          ignoreStack.pop();
        }

        continue; // don't include the destructuring in the header
      }

      if (c === ")") {
        // PARENS: Params end.
        return cleanHeader.substring(parensStart).split(",").map(p => p.trim());
      }

      // initializers
      if (c === "=") {
        // Initializer start.
        insideInitializer = true;
        continue; // don't include initializers in the header
      }
      if (insideInitializer) {
        if (c === ",") {
          // Initializer end.
          insideInitializer = false;
          cleanHeader += c; // include the comma in the header.  it's not part of an initializer, it's the separator between params.
        }

        continue;
      }

      // all other characters are kept
      cleanHeader += c;
    }

    // This should not happen, but might.
    log(`[RuntimeError] extractFunctionParameterNames fell through for: ${s.slice(0, 80)}... header=${cleanHeader}`);
    return [];
  } catch (err) {
    log(`[RuntimeError] extractFunctionParameterNames failed for: ${s.slice(0, 80)}...\n ${err?.stack || err}`);
  }
}

function previewFunction(cdpProperties) {
  const nameProperty = cdpProperties.result.find(prop => prop.name == "name");
  const locationProperty = getInternalFunctionLocationProp(cdpProperties);

  if (nameProperty) {
    // RUN-1991: nameProperty.value might not always exist.
    this.extra.functionName = nameProperty?.value?.value || "";
  }

  if (locationProperty) {
    const loc = locationProperty?.value?.value || "";
    if (!loc) {
      log(`[RUN-1991] previewFunction missing location: ${JSON.stringify(nameProperty)}, ${JSON.stringify(locationProperty)}`);
    }
    this.extra.functionLocation = createProtocolLocation(loc);
  }

  if (this.cdpObj.description) {
    this.extra.functionParameterNames = extractFunctionParameterNames(this.cdpObj.description);
  }
}

const CustomPreviewers = {
  Array: [previewArray],
  Int8Array: [previewTypedArray],
  Uint8Array: [previewTypedArray],
  Uint8ClampedArray: [previewTypedArray],
  Int16Array: [previewTypedArray],
  Uint16Array: [previewTypedArray],
  Int32Array: [previewTypedArray],
  Uint32Array: [previewTypedArray],
  Float32Array: [previewTypedArray],
  Float64Array: [previewTypedArray],
  BigInt64Array: [previewTypedArray],
  BigUint64Array: [previewTypedArray],
  Map: [previewSetMap],
  WeakMap: [previewSetMap],
  Set: [previewSetMap],
  WeakSet: [previewSetMap],
  RegExp: [previewRegExp],
  Date: [previewDate],
  Error: ErrorProperties,
  EvalError: ErrorProperties,
  RangeError: ErrorProperties,
  ReferenceError: ErrorProperties,
  SyntaxError: ErrorProperties,
  TypeError: ErrorProperties,
  URIError: ErrorProperties,
  Function: [previewFunction],
  AsyncFunction: [previewFunction],
};

function createProtocolPropertyDescriptor(desc) {
  const { name, value, writable, get, set, configurable, enumerable, symbol } = desc;

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

  if (symbol) {
    rv.isSymbol = true;
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
