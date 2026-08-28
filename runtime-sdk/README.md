# @prismatix/runtime

TypeScript declarations and a thin ergonomic facade for PrismatiX JavaScript extensions.

The shipped game still runs the native C++ Runtime with its embedded JavaScript engine. This package is authoring/build-time tooling only; it does not add Node.js or a browser runtime to the Player.

```ts
import {createPrismatiXContext, defineCommand} from "@prismatix/runtime";

const ctx = createPrismatiXContext();

defineCommand<{amount: number}>("game.raiseAffection", async ({amount}) => {
  const current = Number(ctx.variables.get("affection") ?? 0);
  ctx.variables.set("affection", current + amount);
  await ctx.time.wait(0.1);
});
```

For code that prefers the host API directly, importing the package's types also declares the existing `Engine`, `px`, and `DisplayMode` globals.

## Build-time bundling

The embedded JavaScript host executes extension entry scripts; it is not a Node.js module loader and does not resolve npm packages at runtime. TypeScript extensions may import `@prismatix/runtime` while authoring, but the project build step must transpile/bundle those imports into the JavaScript shipped with the game.

In other words:

- authoring/build machine: TypeScript + `@prismatix/runtime` imports are fine;
- packaged extension: ship bundled JavaScript with no unresolved npm imports;
- native Player / WASM Preview: execute that JavaScript through the same embedded ScriptHost API.

The facade deliberately wraps the existing host instead of defining a second scripting runtime. Native Player and WASM Preview therefore keep identical ScriptHost semantics.
