'use strict';

require('../common');

const emptyWasmModule = Buffer.from([
  0x00, 0x61, 0x73, 0x6d,
  0x01, 0x00, 0x00, 0x00,
]);

new WebAssembly.Module(emptyWasmModule);
process.exit(0);
