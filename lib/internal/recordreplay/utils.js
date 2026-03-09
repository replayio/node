
function assert(v) {
  if (!v) {
    log(`Assertion failed ${Error().stack}`);
    throw new Error("Assertion failed");
  }
}

function log(text) {
  process.recordreplay.log(text);
}

module.exports = { assert, log };
