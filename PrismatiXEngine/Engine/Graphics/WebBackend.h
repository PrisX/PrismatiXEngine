#pragma once

// The Web Player has two renderers, chosen when it is built
// (PRISMATIX_WEB_RENDERER, see docs/web-export.md):
//
// - webgl2: SDL's WebGL2 renderer plus a separate effects layer
//   (Platform/Web/WebEffects and the export's effects.mjs).
// - webgpu (PRISMATIX_WEB_WEBGPU): the desktop path. One SDL_GPU device, on
//   the WebGPU backend of the pinned SDL fork, is shared by SDL's GPU 2D
//   renderer and the compositor's render states.
//
// Desktop and Android builds define neither macro below.
#if defined(PRISMATIX_WEB_PLAYER) && !defined(PRISMATIX_WEB_WEBGPU)
#define PRISMATIX_WEB_GL_EFFECTS 1
#endif
// Builds without SDL_GPU: the WebGL2 Web Player and the WASM Preview.
#if defined(__EMSCRIPTEN__) && !defined(PRISMATIX_WEB_WEBGPU)
#define PRISMATIX_NO_SDL_GPU 1
#endif
