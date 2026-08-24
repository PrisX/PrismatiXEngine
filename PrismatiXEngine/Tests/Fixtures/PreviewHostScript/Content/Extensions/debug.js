Engine.RegisterCommand("debug.command", async (args) => {
    const amount = args.amount;
    if (args.mode !== "preview" || args.enabled !== true ||
        args.asset.id !== "33333333-3333-4333-8333-333333333333" ||
        args.asset.path !== "Assets/rin.png") {
        throw new Error("typed command arguments were not normalized");
    }
    await Engine.DebugPoint("Content/Extensions/debug.js", 3,
                            { amount, args }, "debugCommand");
    Engine.log("js-print", amount);
    await Engine.DebugPoint("Content/Extensions/debug.js", 4,
                            { amount, args }, "debugCommand");
    const stepped = amount + (args.enabled ? 1 : 0);
    console.warn("js-warn", stepped);
    await Engine.WaitSeconds(0);
    return stepped;
});

Engine.RegisterAction("debug.typed-action", async (args, context) => {
    if (args.mode !== "preview" || args.enabled !== true || context.preview !== true) {
        throw new Error("typed Action arguments were not normalized");
    }
    if (context.scene === "Content/UI/ActionSignalScene.pxui") {
        if (context.node !== "14141414-1414-4414-8414-141414141414" ||
            context.signal !== "studioUi.activated") {
            throw new Error("UI Action context was not preserved");
        }
    } else if (context.scene !== "typed-action.pxir") {
        throw new Error("Runtime IR Action scene was not preserved");
    }
    await Engine.DebugPoint("Content/Extensions/debug.js", 18,
                            { args, context }, "typedAction");
    Engine.log("typed-action-start", args.amount, args.mode);
    await Engine.DebugPoint("Content/Extensions/debug.js", 19,
                            { args, context }, "typedAction");
    await Engine.WaitSeconds(0);
    Engine.SetVariable("typed_action_result", args.amount * 2);
    Engine.log("typed-action-complete", args.amount * 2);
});
