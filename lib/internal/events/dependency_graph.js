'use strict';

// Event dispatches in the Replay dependency graph (src/node_dependency_graph.h).
// Pre-execution installs these over the unmodified EventEmitter#emit and
// EventTarget[kHybridDispatch], only when instrumented. A snapshot holds the
// originals in the slots that still hold these.
//
// Contract: a call runs the original in an emitEvent{type} dispatch iff type is
// a string or symbol and the emitter or target has >= 1 listener for type at
// call time. Deciding it performs only reads that the original also performs,
// by string or symbol keys.

const {
  ArrayIsArray,
  FunctionPrototypeCall,
  ReflectApply,
  SymbolFor,
} = primordials;

const EventEmitter = require('events');
const { EventTarget, kEvents } = require('internal/event_target');
const {
  dependencyGraphBeginDispatch,
  dependencyGraphEndDispatch,
} = internalBinding('async_wrap');
const { dependencyGraphInstrumented } = require('internal/async_hooks');
const {
  addAfterUserSerializeCallback,
  namespace: { isBuildingSnapshot },
} = require('internal/v8/startup_snapshot');

const kHybridDispatch = SymbolFor('nodejs.internal.kHybridDispatch');

const originalEmit = EventEmitter.prototype.emit;
const originalHybridDispatch = EventTarget.prototype[kHybridDispatch];

function isDispatchType(type) {
  return typeof type === 'string' || typeof type === 'symbol';
}

// _events[type]: a listener or an array of listeners (events.js _addListener).
function emitterHasListener(emitter, type) {
  const listeners = emitter._events?.[type];
  return typeof listeners === 'function' ||
    (ArrayIsArray(listeners) && listeners.length !== 0);
}

// [kEvents].get(type): the root of a linked list of listeners.
function targetHasListener(target, type) {
  return target[kEvents]?.get(type)?.next !== undefined;
}

// Gated as well: user snapshot code can hold a wrapper past its uninstall.
function emit(type) {
  if (!dependencyGraphInstrumented() || !isDispatchType(type) ||
      !emitterHasListener(this, type))
    return ReflectApply(originalEmit, this, arguments);
  dependencyGraphBeginDispatch(type);
  try {
    return ReflectApply(originalEmit, this, arguments);
  } finally {
    dependencyGraphEndDispatch();
  }
}

function hybridDispatch(nodeValue, type, event) {
  if (!dependencyGraphInstrumented() || !isDispatchType(type) ||
      !targetHasListener(this, type))
    return FunctionPrototypeCall(originalHybridDispatch, this, nodeValue, type, event);
  dependencyGraphBeginDispatch(type);
  try {
    return FunctionPrototypeCall(originalHybridDispatch, this, nodeValue, type, event);
  } finally {
    dependencyGraphEndDispatch();
  }
}

// Slots that user code replaced keep their values: wrappers there that enclose
// these reach only the originals when not instrumented.
function uninstall() {
  if (EventEmitter.prototype.emit === emit)
    EventEmitter.prototype.emit = originalEmit;
  if (EventTarget.prototype[kHybridDispatch] === hybridDispatch)
    EventTarget.prototype[kHybridDispatch] = originalHybridDispatch;
}

function setupDependencyGraphDispatch() {
  EventEmitter.prototype.emit = emit;
  EventTarget.prototype[kHybridDispatch] = hybridDispatch;
  // After user serialize callbacks, so that their emits dispatch. Pre-execution
  // installs these again at deserialization, if instrumented.
  if (isBuildingSnapshot())
    addAfterUserSerializeCallback(uninstall);
}

module.exports = {
  setupDependencyGraphDispatch,
};
