import {parentPort, workerData} from "node:worker_threads";

import {validateProjectStories, type ProjectValidationRequest} from "./project-validation.js";

try {
  const result = await validateProjectStories(workerData as ProjectValidationRequest);
  parentPort?.postMessage({result});
} catch (error) {
  parentPort?.postMessage({error: error instanceof Error ? error.stack ?? error.message : String(error)});
}
