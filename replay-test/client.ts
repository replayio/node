/* Copyright 2021 Record Replay Inc. */

// Simple protocol client for use in writing standalone applications.

import * as WebSocket from "ws";
import {
  CommandMethods,
  CommandParams,
  CommandResult,
  EventMethods,
  EventParams,
  EventListeners,
} from "@recordreplay/protocol";
import { assert, Deferred, defer } from "./utils";

export type ClientCallbacks = {
  onClose: (code: number, reason: string) => void;
  onError: (e: unknown) => void;
};

export default class ProtocolClient {
  socket: WebSocket;
  callbacks: ClientCallbacks;

  // Internal state.
  openWaiter: Deferred<void> = defer();
  eventListeners: Partial<EventListeners> = {};
  pendingMessages = new Map<number, Deferred<CommandResult<CommandMethods>>>();
  nextMessageId = 1;

  constructor(address: string, callbacks: ClientCallbacks) {
    this.socket = new WebSocket(address);
    this.callbacks = callbacks;

    this.socket.on("open", this.openWaiter.resolve);
    this.socket.on("close", callbacks.onClose);
    this.socket.on("error", callbacks.onError);
    this.socket.on("message", message => this.onMessage(message));
  }

  close() {
    this.socket.close();
  }

  addEventListener<M extends EventMethods>(
    event: M,
    listener: (params: EventParams<M>) => void
  ) {
    this.eventListeners[event] = listener;
  }

  waitUntilOpen() {
    return this.openWaiter.promise;
  }

  async sendCommand<M extends CommandMethods>(
    method: M,
    params: CommandParams<M>,
    sessionId?: string,
    pauseId?: string
  ): Promise<CommandResult<M>> {
    const id = this.nextMessageId++;
    this.socket.send(JSON.stringify({ id, method, params, sessionId, pauseId }));
    const waiter = defer<CommandResult<M>>();
    this.pendingMessages.set(id, waiter);
    return waiter.promise;
  }

  onMessage(str: string) {
    const msg = JSON.parse(str);
    if (msg.id) {
      const { resolve, reject } = this.pendingMessages.get(msg.id as number)!;
      this.pendingMessages.delete(msg.id as number);
      if (msg.result) {
        resolve(msg.result as any);
      } else {
        reject(msg.error);
      }
    } else {
      assert(typeof msg.method === "string");
      assert(typeof msg.params === "object" && msg.params);

      const handler = this.eventListeners[msg.method as EventMethods];
      if (handler) {
        handler({ ...msg.params } as any);
      } else {
        console.error("MissingMessageHandler", { method: msg.method });
      }
    }
  }
}
