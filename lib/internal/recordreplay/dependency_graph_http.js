'use strict';

// HTTP server requests in the Replay dependency graph
// (src/node_dependency_graph.h): a request's dispatch runs in its requestStart
// node, its complete message adds a requestEnd node, and both requestEnd and
// its response's nodes get a request edge from it. A message its parser drops
// before it completes adds none. Callers gate on
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
  dependencyGraphRequestAbandoned,
  dependencyGraphRequestData,
  dependencyGraphRequestEnd,
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

// The most rawHeaders elements (name, value, ...) the socket's parser keeps
// (lib/_http_common.js parserOnHeaders, parserOnHeadersComplete).
function headerElementLimit(socket) {
  const parser = ownValue(socket, 'parser');
  return typeof parser === 'object' && parser !== null ?
    ownValue(parser, 'maxHeaderPairs') :
    undefined;
}

function beginServerRequest(req, socket) {
  requests.set(req, dependencyGraphBeginRequest(ownValue(req, 'method'),
                                                ownValue(req, 'url'),
                                                ownValue(req, 'rawHeaders'),
                                                headerElementLimit(socket)));
}

// req: the parser's IncomingMessage (lib/_http_common.js parserOnBody,
// parserOnMessageComplete), also a client's response, which has no node.
function requestData(req, chunk) {
  dependencyGraphRequestData(requests.get(req) ?? 0, chunk);
}

function requestEnd(req) {
  dependencyGraphRequestEnd(requests.get(req) ?? 0);
}

// req: the IncomingMessage of a parser being freed (lib/_http_common.js
// freeParser), or null. Its message, if not complete, never will be.
function requestAbandoned(req) {
  dependencyGraphRequestAbandoned(requests.get(req) ?? 0);
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
  requestAbandoned,
  requestData,
  requestEnd,
  responseStart,
  responseData,
  responseEnd,
};
