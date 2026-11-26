'use strict';

// HTTP server requests in the Replay dependency graph
// (src/node_dependency_graph.h): a request's dispatch runs in its requestStart
// node, and each part of its message is handed to the program in a node of its
// own: a body chunk in a requestData node, its end in a requestEnd node. The
// parts and its response's nodes get a request edge from it. A message its
// parser drops before it completes adds no requestEnd. Callers gate on
// dependencyGraphInstrumented(), except of takeWireIds and withoutWireIds.
// Node ids differ between recording and replaying, so they stay in this
// script, which is not instrumented, and every function here returns the same
// value in both.

const {
  ArrayPrototypePush,
  ArrayPrototypeSlice,
  ObjectGetOwnPropertyDescriptor,
  SafeWeakMap,
  StringPrototypeToLowerCase,
} = primordials;

const {
  dependencyGraphBeginRequest,
  dependencyGraphBeginRequestData,
  dependencyGraphBeginRequestEnd,
  dependencyGraphEndDispatch,
  dependencyGraphRequestAbandoned,
  dependencyGraphRequestBodyBytes,
  dependencyGraphResponseData,
  dependencyGraphResponseEnd,
  dependencyGraphResponseStart,
} = internalBinding('async_wrap');

// The request header a recording Chromium's network service sends, a value
// per wire attempt (replayio/chromium net/http/http_network_transaction.cc),
// which pairs the request with the client's request. Removed from a server's
// requests while recording or replaying, before the program sees any header.
const kWireIdHeader = 'x-replay-wire-id';

// IncomingMessage -> its requestStart node.
const requests = new SafeWeakMap();

// IncomingMessage -> the x-replay-wire-id values its parser removed.
const wireIds = new SafeWeakMap();

let recordingOrReplaying;

// Not at the top level, which ShadowRealms load as well.
function isRecordingOrReplaying() {
  recordingOrReplaying ??=
    !!internalBinding('process_methods').isRecordingOrReplaying();
  return recordingOrReplaying;
}

function isWireIdHeader(name) {
  return name.length === kWireIdHeader.length &&
    StringPrototypeToLowerCase(name) === kWireIdHeader;
}

// headers: [name, value, ...] elements as received. kept: those without
// x-replay-wire-id; headers itself if none is. ids: its values, in order.
function splitWireIds(headers) {
  const ids = [];
  let kept = headers;
  for (let i = 0; i < headers.length; i += 2) {
    if (isWireIdHeader(headers[i])) {
      if (kept === headers)
        kept = ArrayPrototypeSlice(headers, 0, i);
      ArrayPrototypePush(ids, headers[i + 1]);
    } else if (kept !== headers) {
      ArrayPrototypePush(kept, headers[i], headers[i + 1]);
    }
  }
  return { kept, ids };
}

// A server request's header elements without x-replay-wire-id, its values kept
// for its requestStart. req: the parser's IncomingMessage (lib/_http_common.js
// parserOnHeadersComplete).
function takeWireIds(req, headers) {
  if (!isRecordingOrReplaying())
    return headers;
  const { kept, ids } = splitWireIds(headers);
  wireIds.set(req, ids);
  return kept;
}

// An HTTP/2 server stream's header elements without x-replay-wire-id
// (lib/internal/http2/core.js onSessionHeaders). No requestStart reports it.
function withoutWireIds(headers) {
  return isRecordingOrReplaying() ? splitWireIds(headers).kept : headers;
}

// Accessors are not called: uninstrumented runs don't call them here.
function ownValue(object, key) {
  return ObjectGetOwnPropertyDescriptor(object, key)?.value;
}

// The most rawHeaders elements (name, value, ...) the socket's parser keeps
// (lib/_http_common.js parserOnHeaders, parserOnHeadersComplete), less the
// removed x-replay-wire-id elements, which a fragmented head counted against
// it: rawHeaders reaching it then still tells the parser may have dropped some.
function headerElementLimit(socket, ids) {
  const parser = ownValue(socket, 'parser');
  const limit = typeof parser === 'object' && parser !== null ?
    ownValue(parser, 'maxHeaderPairs') :
    undefined;
  return typeof limit === 'number' && limit > 0 && ids ?
    limit - 2 * ids.length :
    limit;
}

function beginServerRequest(req, socket) {
  const ids = wireIds.get(req);
  requests.set(req, dependencyGraphBeginRequest(ownValue(req, 'method'),
                                                ownValue(req, 'url'),
                                                ownValue(req, 'rawHeaders'),
                                                headerElementLimit(socket, ids),
                                                ids));
}

// The bytes of a body chunk as received, for requestEnd, also if the chunk is
// dumped. req: the parser's IncomingMessage (lib/_http_common.js
// parserOnBody), also a client's response, which has no node.
function requestBodyBytes(req, chunk) {
  dependencyGraphRequestBodyBytes(requests.get(req) ?? 0, chunk);
}

// Begin the hand-over of a part of a message to the program
// (lib/internal/recordreplay/network.js httpDeliveryBegin): a body chunk, or
// the message's end. Each returns whether key is a server request: only then
// it began a dispatch, which the caller ends.
function beginRequestData(key, chunk) {
  if (!requests.has(key))
    return false;
  dependencyGraphBeginRequestData(requests.get(key), chunk);
  return true;
}

function beginRequestEnd(key) {
  if (!requests.has(key))
    return false;
  dependencyGraphBeginRequestEnd(requests.get(key));
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
  takeWireIds,
  withoutWireIds,
  wrapParserOnIncoming,
  beginRequestData,
  beginRequestEnd,
  requestAbandoned,
  requestBodyBytes,
  responseStart,
  responseData,
  responseEnd,
};
