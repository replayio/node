'use strict';

// HTTP server requests in the Replay dependency graph
// (src/node_dependency_graph.h): a request's dispatch runs in its requestStart
// node, and its response's nodes get a request edge from it. Callers gate on
// dependencyGraphInstrumented(). Node ids differ between recording and
// replaying, so they stay in this script, which is not instrumented, and every
// function here returns the same value in both.

const {
  ObjectGetOwnPropertyDescriptor,
  SafeWeakMap,
} = primordials;

const {
  dependencyGraphBeginRequest,
  dependencyGraphEndDispatch,
  dependencyGraphResponseData,
  dependencyGraphResponseEnd,
  dependencyGraphResponseStart,
} = internalBinding('async_wrap');

// IncomingMessage -> its requestStart node.
const requests = new SafeWeakMap();

// Accessors are not called: uninstrumented runs don't call them here.
function ownValue(object, key) {
  return ObjectGetOwnPropertyDescriptor(object, key)?.value;
}

function beginServerRequest(req) {
  requests.set(req, dependencyGraphBeginRequest(ownValue(req, 'method'),
                                                ownValue(req, 'url')));
}

// Ends the dispatch parserOnIncoming began, if it began one.
function wrapParserOnIncoming(parserOnIncoming) {
  return function parserOnIncomingInDependencyGraph(server, socket, state,
                                                    req, keepAlive) {
    try {
      return parserOnIncoming(server, socket, state, req, keepAlive);
    } finally {
      if (requests.has(req))
        dependencyGraphEndDispatch();
    }
  };
}

function requestOf(res) {
  return requests.get(ownValue(res, 'req')) ?? 0;
}

function responseStart(res, status) {
  dependencyGraphResponseStart(status, requestOf(res));
}

function responseData(res, length) {
  dependencyGraphResponseData(length, requestOf(res));
}

function responseEnd(res) {
  dependencyGraphResponseEnd(requestOf(res));
}

module.exports = {
  beginServerRequest,
  wrapParserOnIncoming,
  responseStart,
  responseData,
  responseEnd,
};
