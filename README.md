<p align="center">
  <img src="https://cdn.owen0924.com/images/PrismatiXEngine_Logo.png" alt="PrismatiXEngine Logo">
</p>

<h1 align="center">PrismatiXEngine</h1>

<p align="center">
  <strong>A visual novel engine built in C++, with Lua-driven UI and gameplay logic.</strong>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-20-blue.svg?style=flat-square&logo=c%2B%2B" alt="C++20">
  <img src="https://img.shields.io/badge/Lua-5.4-blue.svg?style=flat-square&logo=lua" alt="Lua 5.4">
  <img src="https://img.shields.io/badge/Render-SDL3-green.svg?style=flat-square&logo=sdl" alt="SDL3">
  <img src="https://img.shields.io/badge/License-MIT-lightgrey.svg?style=flat-square" alt="MIT License">
</p>

---

## Overview

PrismatiXEngine is a modern visual novel game engine built for flexibility and performance. It uses C++ as its core and Lua for the game loop, UI, and more.

## Features

- **Advanced Rendering**: Built on SDL3, supporting sprite animations, transitions, and layer management.
- **Scriptable Architecture**: Entire game logic and UI framework is written in Lua, allowing for more extensibility.
- **Visual Novel Toolkit**: Dialogue systems, choice logic, and a custom script parser (VNScript).
- **Multimedia Integration**: Support for music, sound effects, and voice via SDL3_mixer.
- **Resource Management**: Efficient asset loading with LRU caching and state management.
- **Modular UI Framework**: A component-based UI system implemented in Lua.
- **Cross-Platform**: Designed to run on multiple platforms via SDL3.

## Project Structure

```
PrismatiXEngine/
├── Core/                   # Core C++ Engine Implementation
│   ├── Lua/                # Lua API Bindings (Render, Audio, VN, etc.)
│   ├── Models/             # Data Models (Save, VNCommand)
│   ├── Services/           # Backend Services (ResourceManager, SaveManager)
│   ├── Systems/            # Low-level Systems (RenderSystem, AudioSystem)
│   └── VN/                 # VN Logic (Dialogue, Parser, Flow Control)
├── Scripts/                # Lua Script Resources
│   ├── common/             # UI Framework and Scene Manager
│   ├── components/         # Game UI Components (DialogueBox, Menus)
│   ├── fx/                 # Visual Effects (Transitions, Screen FX)
│   └── scenes/             # Game Scene Definitions
├── Utils/                  # General Utilities (Logger, Cache, Easing)
│
└── main.cpp                # Application Entry Point
```

## Requirements

Ensure the following tools and libraries are installed before building:

- **Compiler**: C++20 compatible (GCC 10+, Clang 10+, or MSVC 2019+)
- **Build System**: CMake 3.21 or higher
- **Dependencies**:
  - SDL3 (including Image, TTF, and Mixer)
  - Lua 5.4+
  - sol2 (Lua bindings for C++)
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
brew install cmake ninja lua@5.4 sdl3 sdl3_image sdl3_ttf sdl3_mixer nlohmann-json spdlog mbedtls zstd fmt
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

The editor binary is generated under `out/build/<preset>/PrismatiXEngine/`.

#### Generic CMake build

```bash
mkdir build && cd build
cmake ..
cmake --build . --config Release
```

## Development

All game development in PrismatiXEngine is handled within the `Scripts/` directory.

### Example Scene

A basic scene in `Scripts/scenes/play_scene.lua` looks like this:

```lua
local PlayScene = {}

function PlayScene:init()
    VN.setBackground("bg_city")
end

return PlayScene
```

### UI Customization

UI components are located in `Scripts/components/`. Since these are written in Lua, you can modify the layout and behavior without recompiling the engine.

## License

This project is licensed under the **MIT License**. See the [LICENSE](LICENSE) file for more information.
