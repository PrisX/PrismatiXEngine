// The packaged catalog journey of PrismatiX.Player.CatalogNativeAcceptance
// (Tests/PlayerCatalogNativeAcceptanceTests.cpp builds the fixture and runs
// this through `prismatix test`). It drives PrismatiXPlayerQA over its QA IPC
// and writes the progress markers that test waits for into the Player log.
//
// PRISMATIX_TEST_LOAD_PREVIOUS_SAVE=1 loads slot 2 (a save from the previous
// release) before the save/load checks; PRISMATIX_TEST_EXPECT_REMOVED_RESOURCE=1
// first checks that a signed update hid a removed resource.

/** @param {import("@prismatix/cli/test").PrismatixTestClient} px */
export default async function catalog(px) {
  const check = (condition, message) => {
    if (!condition) throw new Error(message);
  };
  // The Player shows the result of an action over the next frames (a locale's
  // voice, a filled save slot's thumbnail, the typed preview): wait for what
  // the journey asserts instead of reading it in the frame of the action, and
  // fail with what was last seen if it never shows.
  const eventually = async (describe, probe, timeoutMs = 15_000) => {
    const deadline = Date.now() + timeoutMs;
    let seen;
    for (;;) {
      seen = await probe();
      if (seen.ok) return seen.value;
      if (Date.now() >= deadline) throw new Error(`${describe} (last seen: ${JSON.stringify(seen.value)})`);
      await px.sleep(32);
    }
  };

  if (process.env.PRISMATIX_TEST_EXPECT_REMOVED_RESOURCE) {
    const hidden = !(await px.resourceExists("Content/Overlay/player-second.txt"));
    await px.log(`Player E2E removed update resource hidden=${hidden}`);
    check(hidden, "the signed update did not hide its removed resource");
  }

  // The authored title animation runs, then an authored async Action shows
  // an arbitrary authored route (phone) and returns to the title.
  await px.waitFor({appState: "title", uiAnimationStarted: true});
  await px.log("Player Phase 2 authored title animation advanced");
  await px.action("acceptance.phase2-title");
  let state = await px.waitFor({route: "phone"});
  check(state.ui.route === "phone" && state.ui.screen === "custom" && state.ui.root === "PhoneRoot",
    `the phone route did not install its authored scene (ui ${JSON.stringify(state.ui)})`);
  await px.waitFor({route: "title", scriptIdle: true});
  await px.log("Player Phase 2 title async and custom phone route complete");
  // Keyboard focus to Start and accept it, in one frame.
  await px.batch([{op: "input", action: "focus-next"}, {op: "input", action: "accept"}]);

  const gameReady = {appState: "game", cgUnlocked: "ending-rin", scriptIdle: true, dialogueActive: true};
  await px.waitFor(gameReady);
  await px.action("acceptance.phase2-menu");
  state = await px.waitFor({modal: "replay"});
  check(state.ui.route === "replay" && state.ui.screen === "custom" && state.ui.root === "ReplayRoot",
    `the replay route did not install its authored scene (ui ${JSON.stringify(state.ui)})`);
  await px.waitFor({modal: "", scriptIdle: true});

  if (process.env.PRISMATIX_TEST_LOAD_PREVIOUS_SAVE) {
    await px.load(2);
    await px.log("Player E2E previous-version save loaded");
  }
  // A save taken under a modal restores that exact route and UI stack, even
  // after the routes changed in between. All in one frame, as a player's
  // save and load would be.
  const [, , , , , restored] = await px.batch([
    {op: "route", operation: "showModal", name: "replay"},
    {op: "save", slot: 1},
    {op: "route", operation: "closeModal"},
    {op: "route", operation: "push", name: "phone"},
    {op: "load", slot: 1},
    {op: "state"},
  ]);
  check(restored.routes.stack.join() === "hud" && restored.routes.modals.join() === "replay" &&
    restored.ui.route === "replay" && restored.ui.root === "ReplayRoot",
    `save/load did not restore the route and UI stack (${JSON.stringify(restored.routes)}, ui ${restored.ui.route})`);
  await px.route("closeModal");
  await px.log("Player Phase 2 menu async and save/load stack complete");

  await px.action("save.open");
  state = await px.waitUntil((current) => !current.busy.pendingSaveScreen);
  check(state.ui.screen === "save", `the save menu was replaced before the next frame (${state.ui.screen})`);
  await px.log("Player save menu survived a rendered frame");
  const [, , picked] = await px.batch([{op: "save", slot: 2}, {op: "load", slot: 2}, {op: "state"}]);
  check(picked.ui.route === "hud" && picked.modal === "",
    `the save picker leaked into the loaded checkpoint (ui ${picked.ui.route}, modal ${picked.modal})`);
  await px.log("Player save picker checkpoint restored gameplay HUD");

  state = await px.waitFor(gameReady);
  // The current Runtime IR operation is a wait condition (`run --at` lands
  // on it), alone or as one of a list.
  check(state.story.operationId !== "", "the story reports its current operation");
  await px.waitFor({operation: state.story.operationId}, {timeoutMs: 5_000});
  await px.waitFor({operation: ["not-an-operation", state.story.operationId]}, {timeoutMs: 5_000});
  if (state.locales.length > 1) {
    const locale = state.locales[1];
    // The switch is synchronous, and the replay voice it starts is only on
    // show until its playback ends (audio.voice is empty after that, a
    // backlog voice only follows the line once it is read): read the state in
    // the frame of the switch, not in a later round trip.
    const [, switched] = await px.batch([{op: "setting", name: "language", value: locale}, {op: "state"}]);
    state = switched;
    await px.log(`Player E2E locale switched locale=${locale} text=${state.dialogue.text}`);
    const voice = state.audio.voice || state.backlog.lastVoice;
    await px.log(`Player E2E localized media background=${state.story.background} voice=${voice}`);
    state = await px.waitFor(gameReady);
  }

  // The shipped Player has no F2 "reset demo" shortcut.
  const routeName = await px.variable("routeName");
  await px.key("F2");
  await px.waitFor({frames: 2});
  const afterF2 = await px.state();
  check(afterF2.profile.clearCount === state.profile.clearCount &&
    JSON.stringify(await px.variable("routeName")) === JSON.stringify(routeName),
    "the release F2 policy changed persistent or session state");
  await px.log("Player release F2 policy ignored demo reset shortcut");

  // Save management on the open save screen, on free slots only (later runs
  // load slots 1 and 2): save.copy and save.move wait for the next slot the
  // player picks, a move onto a save swaps the two, save.delete empties a
  // slot. Each refreshes saveLoad.slots; a filled slot is revealed over the
  // next two frames.
  await px.action("save.open");
  await px.waitUntil((current) => !current.busy.pendingSaveScreen && current.ui.screen === "save");
  const slotItem = async (argument) => (await px.uiData("saveLoad.slots"))
    .find((item) => item.argument === String(argument));
  const [first, second, third] = (await px.uiData("saveLoad.slots"))
    .filter((item) => !item.occupied && Number(item.argument) > 2).map((item) => item.argument);
  check(third !== undefined && (await slotItem(1))?.occupied && (await slotItem(2))?.occupied,
    "the save screen does not list slots 1 and 2 as saved and three free slots");
  // A change shows in saveLoad.slots over the next frames: wait for it.
  const slotsShow = async (describe, test) => {
    await px.waitFor({frames: 3});
    return await eventually(describe, async () => {
      const slots = await px.uiData("saveLoad.slots");
      const slot = (argument) => slots.find((item) => item.argument === String(argument));
      const pending = {copy: await px.uiData("saveLoad.pending.copy"),
        visible: await px.uiData("saveLoad.pending.visible")};
      return {ok: test(slot, pending), value: {pending,
        slots: slots.map(({argument, occupied, pending: waiting, image}) => ({argument, occupied, waiting, image}))}};
    });
  };
  await px.action("save.copy", 1);
  await slotsShow("save.copy did not wait for its target slot",
    (slot, pending) => pending.copy === true && slot(1)?.pending === true);
  await px.action("save.slot", first);
  await px.action("save.copy", 2);
  await px.action("save.copy", second);
  await slotsShow("save.copy did not copy into the slot picked next",
    (slot, pending) => slot(first)?.occupied && slot(second)?.occupied && pending.visible === false);
  await px.action("save.move", first);
  await px.action("load.slot", second);
  await slotsShow("save.move onto a save did not swap the two",
    (slot) => slot(first)?.occupied && slot(second)?.occupied);
  await px.action("save.move", second);
  await px.action("save.slot", third);
  await slotsShow("save.move into an empty slot did not move the save",
    (slot) => !slot(second)?.occupied && slot(third)?.occupied);
  // px.save outside a batch takes the next Stage frame as its thumbnail.
  await px.save(Number(second));
  await px.action("saveLoad.page", "1");
  await slotsShow("px.save stored no thumbnail", (slot) => slot(second)?.image !== undefined && slot(second).image !== "");
  await px.action("save.delete", second);
  await px.action("save.delete", first);
  await px.action("save.delete", third);
  await slotsShow("save.delete left a slot occupied",
    (slot) => !slot(first)?.occupied && !slot(second)?.occupied && !slot(third)?.occupied);
  await px.log("Player E2E save management copy, move and delete complete");
  await px.route("closeModal");

  // The settings screen types a sample line at the chosen text speed.
  await px.action("settings.open");
  await px.waitFor({modal: "settings"});
  // Sampled until the length changes: a slow frame rate must not hide the typing.
  const lengths = new Set();
  await eventually("settings.preview did not type", async () => {
    lengths.add(String(await px.uiData("settings.preview")).length);
    await px.waitFor({frames: 3});
    return {ok: lengths.size > 1, value: [...lengths]};
  });
  await px.log("Player E2E settings preview typed");
  await px.route("closeModal");
  await px.waitFor({modal: ""});

  // The remapped Gallery shortcut from the saved settings opens the Gallery.
  const gallery = await px.bindings("screen.open.gallery");
  check(JSON.stringify(gallery.keys) === JSON.stringify(["F8"]),
    `the saved Gallery shortcut was not restored (${gallery.keys.join(", ")})`);
  await px.key("F8");
  await px.waitFor({modal: "gallery", uiRoute: "gallery"});
  await px.waitUntil((current) => !current.busy.routePreload);
  const frame = await px.screenshot();
  await px.log(`Player E2E gallery frame width=${frame.width} height=${frame.height} ` +
    `colors=${frame.sampledColors} hash=${frame.hash}`);
  check(frame.valid && frame.width >= 640 && frame.height >= 360 && frame.sampledColors >= 8,
    "the Gallery frame is blank or undersized");

  if ((await px.bindings("acceptance.close-gallery")).exists) await px.key("F6");
  else await px.batch([{op: "input", action: "cancel"}, {op: "key", name: "Escape"}]);
  await px.waitFor({modal: ""});
  await px.log("Player E2E journey complete name=catalog");
}
