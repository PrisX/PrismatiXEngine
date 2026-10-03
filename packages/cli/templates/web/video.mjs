export class WebVideo {
  constructor(manifest, base, onInteractionRequired, onError) {
    this.renditions = new Map(Object.entries(manifest.videos ?? {}));
    this.base = new URL(base);
    this.items = new Map();
    this.next = 1;
    this.onInteractionRequired = onInteractionRequired;
    this.onError = onError;
  }
  unlock() {
    if (!this.context) this.context = new AudioContext();
    return this.context.resume();
  }
  open(path, volume) {
    const rendition = this.renditions.get(path);
    if (!rendition) { this.onError(new Error(`No exported video for ${path}`)); return 0; }
    const video = document.createElement("video");
    video.playsInline = true;
    video.preload = "auto";
    video.crossOrigin = "anonymous";
    video.src = new URL(rendition, this.base).href;
    const item = {video, error: "", desiredPlaying: true};
    const id = this.next++;
    if (this.context) {
      item.source = this.context.createMediaElementSource(video);
      item.gain = this.context.createGain();
      item.gain.gain.value = volume;
      item.source.connect(item.gain).connect(this.context.destination);
    } else video.volume = volume;
    video.addEventListener("error", () => {
      item.error = `Video playback failed: ${video.error?.message ?? path}`;
      this.onError(new Error(item.error));
    });
    this.items.set(id, item);
    this.play(item);
    return id;
  }
  play(item) {
    item.desiredPlaying = true;
    item.video.play().catch(error => {
      if (error.name === "NotAllowedError") this.onInteractionRequired(() => {
        this.unlock();
        if (item.desiredPlaying) this.play(item);
      });
      else if (error.name !== "AbortError") {
        item.error = error.message;
        this.onError(error);
      }
    });
  }
  control(id, operation) {
    const item = this.items.get(id);
    if (!item) return;
    if (operation === "play") this.play(item);
    else {
      item.desiredPlaying = false;
      item.video.pause();
      if (operation === "close") {
        item.source?.disconnect();
        item.gain?.disconnect();
        item.video.removeAttribute("src");
        item.video.load();
        this.items.delete(id);
      }
    }
  }
  element(id) { return this.items.get(id)?.video; }
  error(id) { return this.items.get(id)?.error ?? "Video was closed"; }
  value(id, key) {
    const item = this.items.get(id), video = item?.video;
    if (!video) return key === "failed" ? 1 : 0;
    return ({width: video.videoWidth, height: video.videoHeight, time: video.currentTime,
      ready: video.readyState >= 2, ended: video.ended, failed: !!item.error})[key] ?? 0;
  }
}
