import {fileURLToPath} from "node:url";
import {assertLocalBuildFresh} from "./build-freshness.js";

assertLocalBuildFresh(fileURLToPath(new URL("../..", import.meta.url)), "@prismatix/authoring-sdk");

export {assertLocalBuildFresh, localSourceFingerprint, writeLocalBuildStamp} from "./build-freshness.js";
export * from "./canonical-json.js";
export * from "./contracts.js";
export * from "./localization.js";
export * from "./motion.js";
export * from "./jsx-ui.js";
export * from "./project-resources.js";
export * from "./project-actions.js";
export * from "./project-normalization.js";
export * from "./project.js";
export * from "./portrait.js";
export * from "./resources.js";
export * from "./semantic.js";
export * from "./story.js";
export * from "./story-identity.js";
export * from "./story-external-identity.js";
export * from "./story-project.js";
export * from "./story-project-node.js";
export type * from "./types.js";
