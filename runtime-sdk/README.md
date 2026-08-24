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

For code that prefers the host API directly, importing the package's types also declares the existing `Engine` and `px` globals as `PrismatiXEngine`.

The facade deliberately wraps the existing host instead of defining a second scripting runtime. Native Player and WASM Preview therefore keep identical ScriptHost semantics.
