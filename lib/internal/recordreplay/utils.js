
function assert(v) {
  if (!v) {
    log(`Assertion failed ${Error().stack}`);
    throw new Error("Assertion failed");
  }
}

function log(text) {
  console.debugLog(text);
}

module.exports = { assert, log };
