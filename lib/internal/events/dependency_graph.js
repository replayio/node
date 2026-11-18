'use strict';

// Event dispatches in the Replay dependency graph (src/node_dependency_graph.h):
// EventEmitter#emit and EventTarget[kHybridDispatch] call their listeners in an
// emitEvent node. Pre-execution installs these over the unmodified originals,
// only when instrumented, so that the originals stay free of it.

const {
  FunctionPrototypeCall,
  ReflectApply,
  SymbolFor,
} = primordials;

const EventEmitter = require('events');
const { EventTarget, kEvents } = require('internal/event_target');
const {
  dependencyGraphBeginDispatch,
  dependencyGraphEndDispatch,
} = require('internal/async_hooks');

const kHybridDispatch = SymbolFor('nodejs.internal.kHybridDispatch');

const originalEmit = EventEmitter.prototype.emit;
const originalHybridDispatch = EventTarget.prototype[kHybridDispatch];

// The original dispatches iff type has listeners. emit('error') emits
// kErrorMonitor in the dispatch of 'error'.
function emit(type) {
  const events = this._events;
  if (events === undefined || events[type] === undefined)
    return ReflectApply(originalEmit, this, arguments);
  dependencyGraphBeginDispatch(type);
  try {
    return ReflectApply(originalEmit, this, arguments);
  } finally {
    dependencyGraphEndDispatch();
  }
}

// The original dispatches iff type has listeners: its early return.
function hybridDispatch(nodeValue, type, event) {
  const root = this[kEvents]?.get(type);
  if (root === undefined || root.next === undefined)
    return FunctionPrototypeCall(originalHybridDispatch, this, nodeValue, type, event);
  dependencyGraphBeginDispatch(type);
  try {
    return FunctionPrototypeCall(originalHybridDispatch, this, nodeValue, type, event);
  } finally {
    dependencyGraphEndDispatch();
  }
}

function setupDependencyGraphDispatch() {
  EventEmitter.prototype.emit = emit;
  EventTarget.prototype[kHybridDispatch] = hybridDispatch;
}

module.exports = {
  setupDependencyGraphDispatch,
};
