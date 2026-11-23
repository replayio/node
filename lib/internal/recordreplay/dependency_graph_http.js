'use strict';

// HTTP server requests in the Replay dependency graph
// (src/node_dependency_graph.h): a request's dispatch runs in its requestStart
// node, and each part of its message is handed to the program in a node of its
// own: a body chunk in a requestData node, its end in a requestEnd node. The
// parts and its response's nodes get a request edge from it. A message its
// parser drops before it completes adds no requestEnd. Callers gate on
// dependencyGraphInstrumented(). Node ids differ between recording and
// replaying, so they stay in this script, which is not instrumented, and every
// function here returns the same value in both.

const {
  ObjectGetOwnPropertyDescriptor,
  SafeWeakMap,
} = primordials;

const {
  dependencyGraphBeginRequest,
  dependencyGraphBeginRequestData,
  dependencyGraphBeginRequestEnd,
  dependencyGraphEndDispatch,
  dependencyGraphRequestAbandoned,
  dependencyGraphRequestData,
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

// req: the parser's IncomingMessage (lib/_http_common.js parserOnBody), also
// a client's response, which has no node.
function requestData(req, chunk) {
  dependencyGraphRequestData(requests.get(req) ?? 0, chunk);
}

// Begins the hand-over of a part of a message to the program
// (lib/internal/recordreplay/network.js httpDeliveryBegin): chunk is the body
// chunk handed over, or undefined at the message's end. Returns whether key is
// a server request: only then it began a dispatch, which the caller ends.
function beginRequestPart(key, chunk) {
  if (!requests.has(key))
    return false;
  const request = requests.get(key);
  if (chunk === undefined)
    dependencyGraphBeginRequestEnd(request);
  else
    dependencyGraphBeginRequestData(request, chunk);
  return true;
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
  beginRequestPart,
  requestAbandoned,
  requestData,
  responseStart,
  responseData,
  responseEnd,
};
