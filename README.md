# spritesheetc

Ridiculously fast spritesheet generator written in C++.
Uses [resvg](https://github.com/linebender/resvg) for SVG rendering and [rectpack2D](https://github.com/TeamHypersomnia/rectpack2D) for bin packing.

`spritesheetc` functions as a command-line application, a Node.js library, and a C++ library.
It is a high-performance alternative to the likes of [TexturePacker](https://www.codeandweb.com/texturepacker),
developed primarily as a tool to generate spritesheets compatible with the [Glimmerite](https://github.com/HasangerGames/glimmerite) game engine.
It is designed around rendering SVGs as quickly and accurately as possible.

## Supported formats
Currently, only SVG input is supported.

Three output formats are supported:
- KTX2 UASTC supercompressed with Zstandard
- Lossless WebP
- PNG

The generated JSON is compatible with a number of popular engines, including [Glimmerite](https://github.com/HasangerGames/glimmerite) and [PixiJS](https://github.com/pixijs/pixijs).

## Using the CLI
[Node.js](https://nodejs.org) is required, for which [nvm](https://github.com/nvm-sh/nvm) is the preferred method of installation if you're using Linux or macOS.
```shell
npm i -g spritesheetc
spritesheetc -h
```

## Using the Node.js API

### Installation
```shell
npm i spritesheetc
```

### Usage
```js
// ES6 Modules/TypeScript
import { buildSpritesheets, buildSpritesheetsFromDirectories } from "spritesheetc";

// Traditional Node.js
const { buildSpritesheets, buildSpritesheetsFromDirectories } = require("spritesheetc");

// Basic usage
const result = buildSpritesheets(["file1.svg", "file2.svg"]);
console.log(result); // 

// Advanced usage
buildSpritesheetsFromDirectories(["path/to/images"], {
    // Names of the generated files will start with this
    atlasName: "myAtlas",

    // Generated files will be outputted here
    outputDirectory: "path/to/destination",

    // Generates two formats of atlas, PNG and lossless WebP. Options are "ktx2", "webp", and "png"
    formats: ["png", "webp"],

    // Generates two sizes of atlas for each format, half resolution and full resolution
    resolutions: [0.5, 1],

    // Fastest speed, largest file size. Options are "slow", "medium", and "fast"
    speed: "fast",
});
```
See [index.d.ts](index.d.ts) for details, and [spritesheetc.js](spritesheetc.js) for example usage.

## Using the C++ API

### Installation
Using CMake's [FetchContent](https://cmake.org/cmake/help/latest/module/FetchContent.html):
```cmake
FetchContent_Declare(
    spritesheetc
    GIT_REPOSITORY https://github.com/HasangerGames/spritesheetc.git
)
FetchContent_MakeAvailable(spritesheetc)
target_link_libraries(myapp PRIVATE spritesheetc)
```

Using a Git submodule:
```shell
git submodule add https://github.com/HasangerGames/spritesheetc.git vendored/spritesheetc
```

```cmake
add_subdirectory(vendored/spritesheetc)
target_link_libraries(myapp PRIVATE spritesheetc)
```

Replace `myapp` with the name of your CMake target. Replace `vendored` with the path to the directory containing your project's dependencies.

### Usage
```cpp
#include <spritesheetc.h>

using namespace spritesheetc;

int main() {
    // Basic usage
    buildSpritesheets({"file1.svg", "file2.svg"});
    
    // Advanced usage
    buildSpritesheetsFromDirectories({"path/to/images"}, {
        // Names of the generated files will start with this
        .atlasName = "myAtlas",
        
        // Generated files will be outputted here
        .outputDirectory = "path/to/destination",
        
        // Generates two formats of atlas, PNG and lossless WebP. Options are Ktx2, Webp, and Png
        .formats = {TextureFormat::Png, TextureFormat::Webp},
        
        // Generates two sizes of atlas for each format, half resolution and full resolution
        .resolutions = {0.5f, 1.0f},
        
        // Fastest speed, largest file size. Options are Slow, Medium, and Fast
        .speed = EncoderSpeed::Fast,
    });
}
```
See [spritesheetc.h](spritesheetc/spritesheetc.h) for details, and [SpritesheetcAddon.cpp](spritesheetc-node/SpritesheetcAddon.cpp) for example usage.

## Building from source

### Prerequisites
- Git
- C++23 or later
- CMake 3.31 or later
- Ninja
- Rust
- Node.js

```shell
git clone https://github.com/HasangerGames/spritesheetc.git
cd spritesheetc
pnpm install
```
