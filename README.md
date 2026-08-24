<p align="center">
  <img src="https://cdn.owen0924.com/images/PrismatiXEngine_Logo.png" alt="PrismatiXEngine Logo">
</p>

<h1 align="center">PrismatiXEngine</h1>

<p align="center">
  <strong>A visual novel runtime built in C++ with sandboxed JavaScript extensions.</strong>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-20-blue.svg?style=flat-square&logo=c%2B%2B" alt="C++20">
  <img src="https://img.shields.io/badge/JavaScript-QuickJS--NG-f7df1e.svg?style=flat-square&logo=javascript" alt="QuickJS-NG">
  <img src="https://img.shields.io/badge/Render-SDL3-green.svg?style=flat-square&logo=sdl" alt="SDL3">
  <img src="https://img.shields.io/badge/License-MIT-lightgrey.svg?style=flat-square" alt="MIT License">
</p>

---

## Overview

PrismatiXEngine is a modern visual novel runtime built around typed content contracts, deterministic execution, and an embedded QuickJS-NG scripting sandbox. Player and Preview use the same JavaScript `ScriptHost`, extension manifest, Action, async checkpoint, and debugger contracts.

## Features

- **Advanced Rendering**: Built on SDL3, supporting sprite animations, transitions, and layer management.
- **Sandboxed Extensions**: Typed `.pxextension` manifests load JavaScript without Node.js, host filesystem, process, network, dynamic evaluation, wall-clock, or random APIs.
- **Visual Novel Toolkit**: Dialogue systems, choice logic, and a custom script parser (VNScript).
- **Multimedia Integration**: Support for music, sound effects, and voice via SDL3_mixer.
- **Resource Management**: Efficient asset loading with LRU caching and state management.
- **Modular UI Framework**: Declarative scenes, reusable components, typed Actions, styles, animation, and behavior graphs.
- **Cross-Platform**: Designed to run on multiple platforms via SDL3.

## Project Structure

```
PrismatiXEngine/
├── Applications/           # Player, Native Preview, and WASM Preview entrypoints
├── Engine/
│   ├── Script/             # Language-neutral contracts and QuickJS-NG host
│   ├── Preview/            # Shared Preview protocol and runtime services
│   ├── SDK/                # Public content and packaging contracts
│   ├── UI/                 # Declarative UI, Actions, styles, and behavior
│   └── VN/                 # Dialogue, commands, scenario, and runtime VM
└── Tests/                  # Contract, integration, native acceptance, and soak tests
```

## Requirements

Ensure the following tools and libraries are installed before building:

- **Compiler**: C++20 compatible (GCC 10+, Clang 10+, or MSVC 2019+)
- **Build System**: CMake 3.21 or higher
- **Dependencies**:
  - SDL3 (including Image, TTF, and Mixer)
  - nlohmann-json
  - spdlog (Logging)
  - MbedTLS
  - zstd

## Installation

### 1. Clone the repository

```bash
git clone https://github.com/your-repo/PrismatiXEngine.git
cd PrismatiXEngine
```

### 2. Install macOS dependencies

On macOS, install the development packages with Homebrew:

```bash
brew install cmake ninja sdl3 sdl3_image sdl3_ttf sdl3_mixer nlohmann-json spdlog mbedtls zstd fmt
```

### 3. Build via CMake

#### macOS Debug/development build

```bash
cmake --preset macos-debug
cmake --build --preset macos-debug
```

#### macOS Release build

```bash
cmake --preset macos-release
cmake --build --preset macos-release
```

Runtime and test binaries are generated under `out/build/<preset>/PrismatiXEngine/`.

#### Generic CMake build

```bash
mkdir build && cd build
cmake ..
cmake --build . --config Release
```

## Development

Runtime extensions live under `Content/Extensions/`. Each extension uses a typed `.pxextension` manifest with `language: "javascript"` and a `.js` entry file.

Canonical JSON Schemas live in `Contracts/`. The frontend-neutral TypeScript Authoring SDK in `packages/authoring-sdk/` parses `.pxstory`, validates project documents, and compiles deterministic Runtime IR/source maps. It is a build-time tool and is not embedded in the Player.

```bash
npm ci
npm run check
```

### Example Extension

A basic command extension looks like this:

```javascript
Engine.RegisterCommand("game.toast", (args) => {
    Engine.log(`toast: ${args.message}`);
});
```

### UI Customization

UI is authored through declarative scene/component documents and typed Action bindings. JavaScript extensions can implement custom Commands and Actions without recompiling the engine.

## License

This project is licensed under the **MIT License**. See the [LICENSE](LICENSE) file for more information.
