// Build <buildId>.symbols.tgz for an RBE build, exactly like backend deploy
// does for make builds: buildSymbolsArchive(buildId, out/Release, ["node"])
// in backend src/shared/instanceUtils.ts + generateSymbolFiles in
// src/shared/symbols.ts. Keep the format in sync with those.
// Temporary, like upload-rbe-build.sh: goes away when RBE builds use backend deploy.
//
//   node symbols-archive.js <buildId> <binary> <outputDir>
const fs = require("fs");
const path = require("path");
const { spawnSync } = require("child_process");

const [buildId, binary, outputDir] = process.argv.slice(2);
if (!buildId || !binary || !outputDir) {
  throw new Error("usage: symbols-archive.js <buildId> <binary> <outputDir>");
}

const nm = spawnSync("/usr/bin/nm", [binary], { maxBuffer: 1e100 });
if (nm.status !== 0) {
  throw new Error(`nm failed: ${nm.stderr}`);
}
const symbols = {};
for (const line of nm.stdout.toString().split("\n")) {
  const arr = /^(.*?) [t|T|W] (.*)/.exec(line);
  if (arr) {
    const addr = +`0x${arr[1]}`;
    if (Number.isNaN(addr) && arr[1] != "(nil)") {
      throw new Error(`toNumber failed: "0x${arr[1]}"`);
    }
    symbols[addr] = arr[2];
  }
}

const jsonFileName = `${buildId}.symbols.json`;
const archiveFile = path.join(outputDir, `${buildId}.symbols.tgz`);
fs.writeFileSync(path.join(outputDir, jsonFileName), JSON.stringify({ [path.basename(binary)]: symbols }));
const tar = spawnSync("tar", ["-czf", archiveFile, "-C", outputDir, jsonFileName], { stdio: "inherit" });
fs.unlinkSync(path.join(outputDir, jsonFileName));
if (tar.status !== 0) {
  throw new Error("tar failed");
}
console.log(archiveFile);
