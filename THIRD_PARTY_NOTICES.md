# PrismatiXEngine third-party notices

This inventory applies to the PrismatiXEngine 0.2 runtime and SDK source tree.
Binary distributions must include this file and the dependency copyright files
installed under `share/PrismatiX/licenses/`. Those copied copyright files are
authoritative when their wording differs from this summary.

| Component | Purpose | License | Upstream |
|---|---|---|---|
| SDL 3 | window, input and graphics platform layer | Zlib | https://github.com/libsdl-org/SDL |
| SDL_image | image decoding | Zlib | https://github.com/libsdl-org/SDL_image |
| SDL_ttf | font loading and shaping integration | Zlib | https://github.com/libsdl-org/SDL_ttf |
| SDL_mixer | audio playback and decoding | Zlib | https://github.com/libsdl-org/SDL_mixer |
| SDL_shadercross | offline shader translation | Zlib | https://github.com/libsdl-org/SDL_shadercross |
| FFmpeg | video/audio demuxing and decoding | LGPL-2.1-or-later (configured feature set) | https://ffmpeg.org/ |
| QuickJS-NG | sandboxed JavaScript runtime | MIT | https://github.com/quickjs-ng/quickjs |
| Mbed TLS | cryptography | Apache-2.0 OR GPL-2.0-or-later | https://github.com/Mbed-TLS/mbedtls |
| Zstandard | package compression | BSD-3-Clause OR GPL-2.0-only | https://github.com/facebook/zstd |
| nlohmann/json | JSON parsing | MIT | https://github.com/nlohmann/json |
| spdlog | logging | MIT | https://github.com/gabime/spdlog |
| fmt | formatting | MIT | https://github.com/fmtlib/fmt |
| FriBidi | bidirectional text | LGPL-2.1-or-later | https://github.com/fribidi/fribidi |
| utf8proc | Unicode processing | MIT | https://github.com/JuliaStrings/utf8proc |
| libunibreak | Unicode line breaking | Zlib | https://github.com/adah1972/libunibreak |
| HarfBuzz | text shaping (via SDL_ttf) | MIT | https://github.com/harfbuzz/harfbuzz |
| FreeType | font rasterization (via SDL_ttf) | FTL OR GPL-2.0-or-later | https://freetype.org/ |
| Noto fonts in `Resources/Fonts` | test and authored-content typography | OFL-1.1 | https://github.com/notofonts |

Codec dependencies selected by the platform package manager (for example
libpng, libjpeg-turbo, WebP, Ogg, Vorbis, Opus, FLAC and mpg123) retain their
own licenses. Their exact copyright files are collected from the pinned vcpkg
installation into the runtime bundle. No `ThirdParty/` source is implicitly
licensed by the PrismatiXEngine MIT license.

For vcpkg builds, installation copies every existing `share/*/copyright` from
the selected target triplet, preserving its package directory name. This can
include notices for build tools or libraries that a particular executable does
not use; it intentionally follows the complete installed dependency set copied
by Windows runtime packaging. Examples include Brotli and DirectX Shader
Compiler (`directx-dxc`), whose notices must not disappear when the DLL set grows.
QuickJS-NG's actual `LICENSE*` / `COPYING*` files are copied from the pinned fetched
source into `licenses/quickjs-ng`; they are not inferred from this summary.
This collection does not replace checking redistribution requirements against
the exact binaries, dependency features, and platform used for a release.

This file is an engineering inventory, not legal advice. Release owners are
responsible for checking the exact dependency feature set and all authored or
sample media before redistribution.
