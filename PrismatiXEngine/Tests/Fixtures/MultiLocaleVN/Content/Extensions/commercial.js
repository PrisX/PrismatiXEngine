import {toastPayload} from "./lib/payload.js";

Engine.RegisterCommand("commercial.toast", async ({message}) => {
  await Engine.emit("sample.toast", toastPayload(message));
});
Engine.RegisterCommand("commercial.unlock", ({id}) => Engine.UnlockCG(id));
Engine.RegisterAction("commercial.openGallery", () => Engine.ShowModal("gallery"));
Engine.on("sample.toast", async (payload) => {
  Engine.log("sample.toast", payload);
  await Engine.WaitSeconds(0);
});
