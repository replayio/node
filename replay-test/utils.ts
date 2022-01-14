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
