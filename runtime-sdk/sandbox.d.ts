// Ambient globals of the PrismatiX extension sandbox (QuickJS in the native
// Player and the WASM Preview). An extension's tsconfig lists this file with
// "types": ["@prismatix/runtime/sandbox"]; `prismatix validate` checks
// extension TypeScript against it.
//
// Besides the ECMAScript standard library, the sandbox has `Engine`, its alias
// `px`, `DisplayMode` and this `console`. There is no Node.js or DOM API
// (filesystem, network, process, timers, fetch), `Date`, `eval` and `Function`
// are undefined and `Math.random` throws: use Engine waits and engine-seeded
// operations so saves, rollback and replays stay deterministic.
/// <reference path="./dist/src/index.d.ts" />

declare var console: import("./dist/src/index.js").SandboxConsole;
