'use strict';

// Network data for the recording's Network panel, reported the way Replay's
// Chromium reports it (record_replay_network.cc and the handlers in
// record_replay_interface.cc): the same driver calls and the same events, so
// the backend serves node recordings unchanged.
//
// It covers what node's own network inspection
// (--experimental-network-inspection, lib/internal/inspector/network_*.js)
// covers, but it isn't built on it: that code subscribes to diagnostics
// channels and adds listeners to responses, which programs can observe
// (diagnostics_channel.hasSubscribers(), listenerCount()). The clients call
// the functions here directly instead, keep their state in WeakMaps, and
// change nothing a program can see.
//
// The undici taps are in node's bundled copy (deps/undici), so they see the
// requests its Agent and Client dispatch. The undici npm package shares the
// global dispatcher with it (Symbol.for('undici.globalDispatcher.2')), so the
// package's requests are reported when they go through node's Agent: when
// they use the global dispatcher and node installed it. Requests dispatched
// by the package's own Agent or Client aren't reported: one passed as a
// `dispatcher`, or the global dispatcher when the package installed it first
// or the program set it with setGlobalDispatcher() (which then holds node's
// own fetch() too). Node's network inspection reports those, from the
// diagnostics channels every undici copy publishes. Reporting them here
// would mean instrumenting the agent the package dispatches through.
//
// The driver reads events and body chunks back only while replaying: it asks
// for them with Target.getCurrentNetworkRequestEvent and
// Target.getCurrentNetworkStreamData from inside the calls reporting them.
// So the events are only built then, in those handlers, and recording only
// pays for the calls themselves.

const {
  ArrayIsArray,
  ArrayPrototypePush,
  ObjectKeys,
  SafeWeakMap,
  String,
  StringPrototypeSplit,
  StringPrototypeStartsWith,
  StringPrototypeToLowerCase,
  StringPrototypeTrim,
} = primordials;

const { Buffer } = require('buffer');
const { dependencyGraphInstrumented } = require('internal/async_hooks');

const binding = internalBinding('process_methods');
const { base64Slice, latin1Slice } = internalBinding('buffer');
const {
  dependencyGraphBeginReceiveData,
  dependencyGraphEndDispatch,
  dependencyGraphNetworkRequest,
} = internalBinding('async_wrap');

let enabled;

// Whether this thread reports its network data: while recording or replaying,
// on the main thread, unless the "notify-network" feature is disabled.
function isNetworkCaptureEnabled() {
  if (enabled === undefined) {
    enabled = !!binding.isRecordingOrReplaying() &&
              !!binding.recordReplayNetworkEnabled();
  }
  return enabled;
}

let lastRequestId = 0;

class NetworkRequest {
  constructor() {
    // As Chromium's "<pid>.<inspector id>".
    this.id = `${process.pid}.${++lastRequestId}`;
    this.requestStreamId = undefined;
    this.requestBytes = 0;
    this.responseStreamId = undefined;
    this.responseBytes = 0;
    this.responseStatus = undefined;
    // Returns what the response event describes; only called while replaying.
    this.describeResponse = undefined;
    this.finished = false;
  }
}

// The event the driver is reading, while it is reported.
let currentEvent;
// The chunk of body data the driver is reading, while it is reported.
let currentChunk;

function emitEvent(request, event) {
  currentEvent = event;
  try {
    binding.recordReplayNetworkRequestEvent(request.id);
  } finally {
    currentEvent = undefined;
  }
}

// Reports a new request, at the point the program made it, or for a
// redirect, at the point its client followed the redirect: each request's
// bookmark is its own. `describe` returns the request event's url, method and
// headers, and optionally its requestCause; it only runs while replaying.
function reportRequest(describe, redirectSource) {
  const request = new NetworkRequest();
  binding.recordReplayNetworkRequest(request.id);
  if (dependencyGraphInstrumented())
    dependencyGraphNetworkRequest(request.id);
  emitEvent(request, () => {
    const { url, method, headers, cause } = describe();
    const event = {
      kind: 'request',
      requestUrl: url,
      requestMethod: method,
      requestHeaders: headers,
    };
    if (cause !== undefined) {
      event.requestCause = cause;
    }
    // Only the chain's first request has an initiator: the program made
    // that one, and the client made the redirects. DevTools' Network panel
    // shows a redirect's redirect source in place of an initiator.
    if (redirectSource !== undefined) {
      event.redirectSourceId = redirectSource.id;
    } else {
      const initiator = binding.recordReplayNetworkInitiator();
      if (initiator !== undefined) {
        event.initiator = initiator;
      }
    }
    return event;
  });
  return request;
}

function isRedirectStatus(status) {
  return status === 301 || status === 302 || status === 303 ||
         status === 307 || status === 308;
}

// `describe` returns the response event's headers, protocol version, status
// and status text; it only runs while replaying.
function reportResponse(request, describe) {
  request.describeResponse = describe;
  emitEvent(request, () => {
    const { headers, protocolVersion, status, statusText } = describe();
    return {
      kind: 'response',
      responseHeaders: headers,
      responseProtocolVersion: protocolVersion,
      responseStatus: status,
      responseStatusText: statusText,
      responseFromCache: false,
    };
  });
}

function reportStreamData(streamId, offset, chunk) {
  currentChunk = chunk;
  try {
    binding.recordReplayNetworkStreamData(streamId, offset, chunk.byteLength);
  } finally {
    currentChunk = undefined;
  }
}

// The request body's bytes as the program sent them. A body the program
// compressed itself (a Content-Encoding request header) is reported
// compressed: the backend could decode it from that header, as it decodes
// compressed responses (RequestResponseBodyEvent.contentEncodings), but
// doesn't yet.
function reportRequestData(request, chunk) {
  if (request.finished || chunk.byteLength === 0) {
    return;
  }
  if (request.requestStreamId === undefined) {
    emitEvent(request, () => ({ kind: 'request-body' }));
    request.requestStreamId = `request-${request.id}`;
    binding.recordReplayNetworkStreamStart(
      request.requestStreamId, 'request-data', request.id);
  }
  reportStreamData(request.requestStreamId, request.requestBytes, chunk);
  request.requestBytes += chunk.byteLength;
}

// The content codings of a response's body, in the order they were applied,
// from its Content-Encoding headers. node's clients report the body as it
// arrived, still compressed (fetch() decodes it later, the other clients leave
// that to the program), so the backend decodes it with these
// (RequestResponseBodyEvent.contentEncodings). It falls back to the body as it
// arrived for codings it can't decode, so all of them are listed.
function contentEncodingList(headers) {
  const codings = [];
  for (let i = 0; i < headers.length; i++) {
    if (StringPrototypeToLowerCase(headers[i].name) !== 'content-encoding') {
      continue;
    }
    const values = StringPrototypeSplit(headers[i].value, ',');
    for (let j = 0; j < values.length; j++) {
      const coding = StringPrototypeToLowerCase(StringPrototypeTrim(values[j]));
      if (coding !== '' && coding !== 'identity') {
        ArrayPrototypePush(codings, coding);
      }
    }
  }
  return codings;
}

// Body data is reported as it arrives, not when the program reads it as
// node's network inspection does: a response nobody reads still has its body
// and finishes, at the time its data came in.
function reportResponseData(request, chunk) {
  if (request.finished || chunk.byteLength === 0) {
    return;
  }
  if (request.responseStreamId === undefined) {
    emitEvent(request, () => {
      const event = { kind: 'response-body' };
      const contentEncodings = request.describeResponse === undefined ?
        [] :
        contentEncodingList(request.describeResponse().headers);
      if (contentEncodings.length > 0) {
        event.contentEncodings = contentEncodings;
      }
      return event;
    });
    request.responseStreamId = `response-${request.id}`;
    binding.recordReplayNetworkStreamStart(
      request.responseStreamId, 'response-data', request.id);
  }
  reportStreamData(request.responseStreamId, request.responseBytes, chunk);
  request.responseBytes += chunk.byteLength;
}

function reportFinished(request) {
  if (request.finished) {
    return;
  }
  request.finished = true;
  const encodedBodySize = request.responseBytes;
  emitEvent(request, () => ({ kind: 'request-done', encodedBodySize }));
}

function reportFailed(request, error) {
  if (request.finished) {
    return;
  }
  request.finished = true;
  emitEvent(request, () => {
    const event = { kind: 'request-failed' };
    const reason = describeError(error);
    if (reason !== undefined) {
      event.requestFailedReason = reason;
    }
    return event;
  });
}

// A client hands a part of a request's answer to the program (headers, a body
// chunk, its end or its failure) between the *DeliveryBegin and *DeliveryEnd
// calls of the request's key in `requests`. In the Replay dependency graph
// (src/node_dependency_graph.h) the hand-off runs in a networkReceiveData
// node, so what the program does with the answer is caused by it. Both test
// the same conditions, so that each begin has its end.
function beginDelivery(requests, key, chunk) {
  if (!dependencyGraphInstrumented()) {
    return;
  }
  const request = requests.get(key);
  if (request !== undefined) {
    dependencyGraphBeginReceiveData(request.id, chunk?.byteLength ?? 0);
  }
}

function endDelivery(requests, key) {
  if (dependencyGraphInstrumented() && requests.has(key)) {
    dependencyGraphEndDispatch();
  }
}

// Only reads the error's own data properties, so none of the program's code
// runs while replaying.
function describeError(error) {
  if (error === null || typeof error !== 'object') {
    return undefined;
  }
  const { code, message } = error;
  if (typeof code === 'string') {
    return code;
  }
  return typeof message === 'string' ? message : undefined;
}

// Header lists as the protocol has them, from a { name: value } object whose
// values are strings, numbers or arrays of them.
function headerList(headers, list = []) {
  const names = ObjectKeys(headers);
  for (let i = 0; i < names.length; i++) {
    addHeader(list, names[i], headers[names[i]]);
  }
  return list;
}

function addHeader(list, name, value) {
  if (ArrayIsArray(value)) {
    for (let i = 0; i < value.length; i++) {
      ArrayPrototypePush(list, { name, value: String(value[i]) });
    }
  } else {
    ArrayPrototypePush(list, { name, value: String(value) });
  }
}

// From node's [name, value, name, value, ...] raw header arrays.
function rawHeaderList(rawHeaders) {
  const list = [];
  for (let i = 0; i + 1 < rawHeaders.length; i += 2) {
    ArrayPrototypePush(list, { name: rawHeaders[i], value: rawHeaders[i + 1] });
  }
  return list;
}

// The http and https clients (lib/_http_client.js).

const httpRequests = new SafeWeakMap();

function httpRequestCreated(clientRequest) {
  if (!isNetworkCaptureEnabled()) {
    return;
  }
  const { kOutHeaders } = require('internal/http');
  httpRequests.set(clientRequest, reportRequest(() => {
    const outHeaders = clientRequest[kOutHeaders];
    const headers = [];
    let host;
    if (outHeaders) {
      const keys = ObjectKeys(outHeaders);
      for (let i = 0; i < keys.length; i++) {
        const { 0: name, 1: value } = outHeaders[keys[i]];
        if (keys[i] === 'host') {
          host = value;
        }
        addHeader(headers, name, value);
      }
    }
    const path = clientRequest.path;
    const url =
      StringPrototypeStartsWith(path, 'http://') ||
      StringPrototypeStartsWith(path, 'https://') ?
        path :
        `${clientRequest.protocol}//${host ?? clientRequest.host}${path}`;
    return { url, method: clientRequest.method, headers };
  }));
}

function httpResponseReceived(clientRequest, response) {
  const request = httpRequests.get(clientRequest);
  if (request === undefined) {
    return;
  }
  httpRequests.set(response, request);
  reportResponse(request, () => ({
    headers: rawHeaderList(response.rawHeaders),
    protocolVersion: `http/${response.httpVersion}`,
    status: response.statusCode,
    statusText: response.statusMessage ?? '',
  }));
}

function httpResponseData(response, chunk) {
  const request = httpRequests.get(response);
  if (request !== undefined) {
    reportResponseData(request, chunk);
  }
}

function httpResponseComplete(response) {
  const request = httpRequests.get(response);
  if (request !== undefined) {
    reportFinished(request);
  }
}

function httpRequestFailed(clientRequest, error) {
  const request = httpRequests.get(clientRequest);
  if (request !== undefined) {
    reportFailed(request, error);
  }
}

// key: the ClientRequest or its IncomingMessage.
function httpDeliveryBegin(key, chunk) {
  beginDelivery(httpRequests, key, chunk);
}

function httpDeliveryEnd(key) {
  endDelivery(httpRequests, key);
}

// The undici client (deps/undici/undici.js), which fetch() is built on.

const undiciRequests = new SafeWeakMap();

// The header lists of undici alternate names and values, as strings or as the
// header's bytes.
function headerString(value) {
  return typeof value === 'string' ?
    value :
    latin1Slice(value, 0, value.byteLength);
}

function pairHeaderList(headers) {
  const list = [];
  for (let i = 0; i + 1 < headers.length; i += 2) {
    ArrayPrototypePush(list, {
      name: headerString(headers[i]),
      value: headerString(headers[i + 1]),
    });
  }
  return list;
}

// fetch() dispatches each hop of a redirect chain with a copy of the same
// fetch params, which share their controller: the last request each
// controller dispatched, and the one dispatching.
const fetchRequests = new SafeWeakMap();
let dispatchingFetch;

// The undici client creates the request synchronously, inside dispatch().
function undiciFetchDispatch(fetchParams, dispatch) {
  if (!isNetworkCaptureEnabled()) {
    return dispatch();
  }
  const outerFetch = dispatchingFetch;
  dispatchingFetch = fetchParams.controller;
  try {
    return dispatch();
  } finally {
    dispatchingFetch = outerFetch;
  }
}

function undiciRequestCreated(undiciRequest) {
  // WebSocket handshakes aren't reported, as node's network inspection
  // doesn't report WebSockets' requests either.
  if (!isNetworkCaptureEnabled() || undiciRequest.upgrade) {
    return;
  }
  const fetchController = dispatchingFetch;
  dispatchingFetch = undefined;
  let redirectSource;
  if (fetchController !== undefined) {
    const previous = fetchRequests.get(fetchController);
    if (previous !== undefined && isRedirectStatus(previous.responseStatus)) {
      redirectSource = previous;
    }
  }
  const request = reportRequest(() => ({
    url: `${undiciRequest.origin}${undiciRequest.path}`,
    method: undiciRequest.method,
    headers: pairHeaderList(undiciRequest.headers),
    // Matches upstream node's network inspection, which gives every undici
    // request the Fetch resource type (and http's and http2's Other, so they
    // have no cause). Chromium's cause for fetch() is "fetch" too.
    cause: 'fetch',
  }), redirectSource);
  undiciRequests.set(undiciRequest, request);
  if (fetchController !== undefined) {
    fetchRequests.set(fetchController, request);
  }
}

function undiciRequestBodySent(undiciRequest, chunk) {
  const request = undiciRequests.get(undiciRequest);
  if (request !== undefined) {
    reportRequestData(
      request,
      typeof chunk === 'string' ? Buffer.from(chunk) : chunk);
  }
}

function undiciResponseStarted(undiciRequest, statusCode, headers, statusText) {
  const request = undiciRequests.get(undiciRequest);
  if (request === undefined) {
    return;
  }
  request.responseStatus = statusCode;
  reportResponse(request, () => ({
    headers: ArrayIsArray(headers) ?
      pairHeaderList(headers) :
      headerList(headers),
    // The requests of undici don't know their connection's protocol.
    protocolVersion: 'http/1.1',
    status: statusCode,
    statusText: statusText ?? '',
  }));
}

function undiciResponseData(undiciRequest, chunk) {
  const request = undiciRequests.get(undiciRequest);
  if (request !== undefined) {
    reportResponseData(request, chunk);
  }
}

function undiciResponseEnd(undiciRequest) {
  const request = undiciRequests.get(undiciRequest);
  if (request !== undefined) {
    reportFinished(request);
  }
}

function undiciResponseError(undiciRequest, error) {
  const request = undiciRequests.get(undiciRequest);
  if (request !== undefined) {
    reportFailed(request, error);
  }
}

function undiciDeliveryBegin(undiciRequest, chunk) {
  beginDelivery(undiciRequests, undiciRequest, chunk);
}

function undiciDeliveryEnd(undiciRequest) {
  endDelivery(undiciRequests, undiciRequest);
}

// The http2 client's streams (lib/internal/http2/core.js).

const http2Streams = new SafeWeakMap();

// Regular headers from an http2 header object, without its pseudo-headers
// (":method", ":path", ...), which the event's other fields describe.
function http2HeaderList(headers) {
  const list = [];
  const names = ObjectKeys(headers);
  for (let i = 0; i < names.length; i++) {
    if (!StringPrototypeStartsWith(names[i], ':')) {
      addHeader(list, names[i], headers[names[i]]);
    }
  }
  return list;
}

function http2StreamCreated(stream, headers) {
  if (!isNetworkCaptureEnabled()) {
    return;
  }
  http2Streams.set(stream, reportRequest(() => ({
    url: `${headers[':scheme']}://${headers[':authority']}${headers[':path']}`,
    method: headers[':method'],
    headers: http2HeaderList(headers),
  })));
}

function http2StreamBodySent(stream, writev, data, encoding) {
  const request = http2Streams.get(stream);
  if (request === undefined) {
    return;
  }
  if (!writev) {
    reportRequestData(
      request,
      typeof data === 'string' ? Buffer.from(data, encoding) : data);
    return;
  }
  for (let i = 0; i < data.length; i++) {
    const entry = data[i];
    if (data.allBuffers) {
      reportRequestData(request, entry);
    } else {
      reportRequestData(
        request,
        typeof entry.chunk === 'string' ?
          Buffer.from(entry.chunk, entry.encoding) :
          entry.chunk);
    }
  }
}

function http2ResponseReceived(stream, headers) {
  const request = http2Streams.get(stream);
  if (request === undefined) {
    return;
  }
  reportResponse(request, () => ({
    headers: http2HeaderList(headers),
    // As Chromium names HTTP/2.
    protocolVersion: 'http/2.0',
    status: headers[':status'],
    statusText: '',
  }));
}

function http2ResponseData(stream, chunk) {
  const request = http2Streams.get(stream);
  if (request !== undefined) {
    reportResponseData(request, chunk);
  }
}

// A stream closed with an error code is reported by http2StreamFailed().
function http2StreamClosed(stream, succeeded) {
  const request = http2Streams.get(stream);
  if (request !== undefined && succeeded) {
    reportFinished(request);
  }
}

function http2StreamFailed(stream, error) {
  const request = http2Streams.get(stream);
  if (request !== undefined) {
    reportFailed(request, error);
  }
}

function http2DeliveryBegin(stream, chunk) {
  beginDelivery(http2Streams, stream, chunk);
}

function http2DeliveryEnd(stream) {
  endDelivery(http2Streams, stream);
}

// Replay's command handlers (lib/internal/recordreplay/main.js).

function Target_getCurrentNetworkRequestEvent() {
  if (currentEvent === undefined) {
    return {};
  }
  return { data: currentEvent() };
}

function Target_getCurrentNetworkStreamData({ index, length }) {
  if (currentChunk === undefined ||
      index < 0 || length < 0 || index + length > currentChunk.byteLength) {
    return {};
  }
  return {
    data: {
      kind: 'data',
      value: base64Slice(currentChunk, index, index + length),
    },
  };
}

module.exports = {
  isNetworkCaptureEnabled,
  http2StreamCreated,
  http2StreamBodySent,
  http2ResponseReceived,
  http2ResponseData,
  http2StreamClosed,
  http2StreamFailed,
  http2DeliveryBegin,
  http2DeliveryEnd,
  httpRequestCreated,
  httpResponseReceived,
  httpResponseData,
  httpResponseComplete,
  httpRequestFailed,
  httpDeliveryBegin,
  httpDeliveryEnd,
  undiciFetchDispatch,
  undiciRequestCreated,
  undiciRequestBodySent,
  undiciResponseStarted,
  undiciResponseData,
  undiciResponseEnd,
  undiciResponseError,
  undiciDeliveryBegin,
  undiciDeliveryEnd,
  Target_getCurrentNetworkRequestEvent,
  Target_getCurrentNetworkStreamData,
};
