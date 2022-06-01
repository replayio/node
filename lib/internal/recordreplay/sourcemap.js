const crypto = require("crypto");
const fsPromises = require("fs/promises");
const path = require("path");
const { fileURLToPath, pathToFileURL } = require("url");
const { sendMessage } = require("internal/recordreplay/message");
const { assert, log } = require("internal/recordreplay/utils");

async function registerSourceMap(ev) {
  if (!ev.sourceMapURL) {
    return;
  }

  const { url: sourceURL, scriptId } = ev;
  const sourceBaseURL = sourceURL && isValidBaseURL(sourceURL) ? sourceURL : pathToFileURL(process.cwd());

  let sourceMapURL;
  try {
    sourceMapURL = new URL(ev.sourceMapURL, sourceBaseURL).toString();
  } catch (err) {
    log("Failed to process sourcemap url: " + err.message);
    return;
  }
  if (!sourceMapURL.startsWith("file://")) {
    return;
  }

  const sourceMapPath = fileURLToPath(sourceMapURL);
  let sourceMap;
  try {
    sourceMap = await fsPromises.readFile(sourceMapPath, "utf8");
  } catch (err) {
    log(`Failed to read sourcemap ${sourceMapPath}: ${err.message}`);
  }
  if (!sourceMap) {
    return;
  }

  const script = await sendMessage("Debugger.getScriptSource", { scriptId });

  const recordingId = process.recordreplay.recordingId();
  const id = String(Math.floor(Math.random() * 10000000000));
  const name = `sourcemap-${id}.map`;
  const path = await writeToRecordingDirectory(name, sourceMap);
  await addRecordingEvent({
    kind: "sourcemapAdded",
    path,
    recordingId,
    id,
    url: sourceMapURL,
    baseURL: sourceMapURL,
    targetContentHash: script?.scriptSource ? makeAPIHash(script.scriptSource) : undefined,
    targetURLHash: sourceURL ? makeAPIHash(sourceURL) : undefined,
    targetMapURLHash: makeAPIHash(sourceMapURL),
  });

  const { sources } = collectUnresolvedSourceMapResources(sourceMap, sourceMapURL);

  for (const { offset, url } of sources) {
    const sourcePath = fileURLToPath(url);
    let sourceContent;
    try {
      sourceContent = await fsPromises.readFile(sourcePath, "utf8");
    } catch (err) {
      log(`Failed to read original source ${sourcePath}: ${err.message}`);
      continue;
    }
    const sourceId = String(Math.floor(Math.random() * 10000000000));
    const name = `original-source-${id}-${sourceId}`;
    const path = await writeToRecordingDirectory(name, sourceContent);
    await addRecordingEvent({
      kind: "originalSourceAdded",
      path,
      recordingId,
      parentId: id,
      parentOffset: offset,
    });
  }
}

function isValidBaseURL(url) {
  try {
    new URL("", url);
    return true;
  } catch {
    return false;
  }
}

function makeAPIHash(content) {
  assert(typeof content === "string");
  return "sha256:" + crypto.createHash('sha256').update(content).digest('hex');
}

function getRecordingDirectory() {
  // see matching logic in recordreplay::InitializeRecordingEvents() in the backend,
  // in getRecordingDirectory() in gecko-dev and in getDirectory() in the recordings cli
  const recordingDir = process.env["RECORD_REPLAY_DIRECTORY"];
  if (recordingDir) {
    return recordingDir;
  }
  const homeDir = process.env["HOME"] || process.env["USERPROFILE"];
  if (!homeDir) {
    log("NoRecordingDirectory");
    return undefined;
  }
  return path.join(homeDir, ".replay");
}

async function addRecordingEvent(event) {
  const recordingDir = getRecordingDirectory();
  if (!recordingDir) {
    return;
  }
  const filepath = path.join(recordingDir, "recordings.log");
  await fsPromises.appendFile(filepath, JSON.stringify(event) + "\n");
}

async function writeToRecordingDirectory(filename, contents) {
  const recordingDir = getRecordingDirectory();
  if (!recordingDir) {
    return;
  }
  try {
    await fsPromises.mkdir(recordingDir);
  } catch {}
  const filepath = path.join(recordingDir, filename);
  await fsPromises.writeFile(filepath, contents);
  return filepath;
}

function collectUnresolvedSourceMapResources(mapText, mapURL) {
  let obj;
  try {
    obj = JSON.parse(mapText);
    if (typeof obj !== "object" || !obj) {
      return {
        sources: [],
      };
    }
  } catch (err) {
    log(`Exception parsing sourcemap JSON (${mapURL})`);
    return {
      sources: [],
    };
  }

  function logError(msg) {
    log(`${msg} (${mapURL}:${sourceOffset})`);
  }

  const unresolvedSources = [];
  let sourceOffset = 0;

  if (obj.version !== 3) {
    logError("Invalid sourcemap version");
    return {
      sources: [],
    };
  }

  if (obj.sources != null) {
    const { sourceRoot, sources, sourcesContent } = obj;

    if (Array.isArray(sources)) {
      for (let i = 0; i < sources.length; i++) {
        const offset = sourceOffset++;

        if (
          !Array.isArray(sourcesContent) ||
          typeof sourcesContent[i] !== "string"
        ) {
          let url = sources[i];
          if (typeof sourceRoot === "string" && sourceRoot) {
            url = sourceRoot.replace(/\/?/, "/") + url;
          }
          let sourceURL;
          try {
            sourceURL = new URL(url, mapURL).toString();
          } catch {
            logError("Unable to compute original source URL: " + url);
            continue;
          }
          if(!sourceURL.startsWith("file://")) {
            continue;
          }

          unresolvedSources.push({
            offset,
            url: sourceURL,
          });
        }
      }
    } else {
      logError("Invalid sourcemap source list");
    }
  }

  return {
    sources: unresolvedSources,
  };
}

module.exports = {
  registerSourceMap,
};
