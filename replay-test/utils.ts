import { spawnSync } from "child_process";

export function assert(v: any, why = ""): asserts v {
  if (!v) {
    const error = new Error(`Assertion Failed: ${why}`);
    error.name = "AssertionFailure";
    console.error(error);
    throw error;
  }
}

export type Resolve<T> = (value: T) => void;

export interface Deferred<T> {
  promise: Promise<T>;
  resolve: Resolve<T>;
  reject: (reason: any) => void;
}

export function defer<T>() {
  let resolve!: (value: T) => void;
  let reject!: (reason: any) => void;
  const promise = new Promise<T>((res, rej) => {
    resolve = res;
    reject = rej;
  });
  return { promise, resolve, reject };
}

// Kill all running subprocesses of the current process.
export function killTransitiveSubprocesses() {
  assert(process.platform != "win32", "NYI");

  const childToParent: Map<number, number> = new Map();

  const lines = spawnSync("ps", ["-A", "-o", "ppid,pid"]).stdout.toString().split("\n");
  for (const line of lines) {
    const match = /(\d+)\s+(\d+)/.exec(line);
    if (match && +match[1] > 1) {
      childToParent.set(+match[2], +match[1]);
    }
  }

  for (const childPid of childToParent.keys()) {
    if (shouldKillSubprocess(childPid)) {
      try {
        spawnSync("kill", ["-KILL", childPid.toString()]);
      } catch (e) {}
    }
  }

  function shouldKillSubprocess(childPid: number) {
    while (true) {
      const parent = childToParent.get(childPid);
      if (!parent) {
        return false;
      }
      if (parent == process.pid) {
        return true;
      }
      childPid = parent;
    }
  }
}
