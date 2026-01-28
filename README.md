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
[Node.js](https://nodejs.org) is required, for which [nvm](https://github.com/nvm-sh/nvm) is the preferred method of installation if you're running Linux or macOS.
```shell
$ npm i -g spritesheetc
$ spritesheetc -h
```
```
Usage: spritesheetc [options]

High-performance spritesheet generator

Options:
  -V, --version                       output the version number
  -i, --inputs <files...>             inputs (REQUIRED): .svg files, directories containing .svg files, or .txt lists of files/directories (newline-separated, lines starting with # ignored)
  -o, --output-dir <directory>        directory to output atlases to (default: "output")
  -n, --atlas-name <name>             name of output atlases (default: "atlas")
  -f, --formats <formats...>          output texture formats (choices: "ktx2", "webp", "png", default: ["webp"])
  -r, --resolutions <resolutions...>  output resolutions, must be between 0.25-1 (default: [1])
  -s, --speed <speed>                 encoding speed: slow means smaller files, fast means larger files (choices: "slow", "medium", "fast", default: "medium")
  -a, --max-atlas-size <size>         max size of each atlas texture, cannot be greater than 16384 (default: 4096)
  -t, --power-of-two                  ensures atlas dimensions are powers of two (default: false)
  -u, --square                        ensures atlas width and height are equal (default: false)
  -x, --fixed-size                    forces all atlases to max-atlas-size (default: false)
  -p, --padding <pixels>              adds padding around sprites to prevent pixels leaking (default: 2)
  -l, --no-allow-rotation             disables rotating sprites 90 degrees to save atlas space
  -g, --no-allow-trimming             disables removing transparent pixels from the edges of sprites to save atlas space
  -e, --extension <extension>         extension to add to sprite names (default: "")
  -z, --max-output-dir-size <size>    max size of output directory in bytes, deleting old atlases if exceeded (default: 500,000,000 = 500 MB)
  -c, --no-cache                      disables cache
  -q, --no-log-status                 disables logging
  -m, --no-multithreaded              disables multithreading
  -h, --help                          display help for command
```

## Using the Node.js API

### Installation
```shell
$ npm i spritesheetc
```

### Usage
```js
// ES6 Modules/TypeScript
import { buildSpritesheets } from "spritesheetc";

// Traditional Node.js
const { buildSpritesheets } = require("spritesheetc");

const files = buildSpritesheets({
    // List of .svg files, directories containing .svg files, or .txt lists of files/directories
    inputs: ["path/to/images"],

    // Generated files will be outputted here
    outputDir: "path/to/destination",

    // Names of the generated files will start with this
    atlasName: "myAtlas",

    // Generates two formats of atlas, PNG and WebP. Options are "ktx2", "webp", and "png"
    formats: ["png", "webp"],

    // Generates two sizes of atlas for each format, half resolution and full resolution
    resolutions: [0.5, 1],

    // Fastest speed, largest file size. Options are "slow", "medium", and "fast"
    speed: "fast",
});
console.log(files);

// Example output:
// [
//   'output/atlas@1x-396de2aad2cb2bc6-1.webp',
//   'output/atlas@1x-396de2aad2cb2bc6-1.png'
// ]
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
$ git submodule add https://github.com/HasangerGames/spritesheetc.git vendored/spritesheetc
```

```cmake
add_subdirectory(vendored/spritesheetc)
target_link_libraries(myapp PRIVATE spritesheetc)
```

Replace `myapp` with the name of your CMake target. Replace `vendored` with the path to the directory containing your project's dependencies.

### Usage
```cpp
#include <vector>
#include <string>
#include <iostream>
#include <spritesheetc.h>

using namespace spritesheetc;

int main() {
    std::vector<std::string> files = buildSpritesheets({
        // List of .svg files, directories containing .svg files, or .txt lists of files/directories
        .inputs = {"path/to/images"},
    
        // Generated files will be outputted here
        .outputDir = "path/to/destination",
        
        // Names of the generated files will start with this
        .atlasName = "myAtlas",
        
        // Generates two formats of atlas, PNG and WebP. Options are Ktx2, Webp, and Png
        .formats = {TextureFormat::Png, TextureFormat::Webp},
        
        // Generates two sizes of atlas for each format, half resolution and full resolution
        .resolutions = {0.5f, 1.0f},
        
        // Fastest speed, largest file size. Options are Slow, Medium, and Fast
        .speed = EncoderSpeed::Fast,
    });
    
    for (const std::string& file : files) {
        std::cout << file << '\n';
    }
    
    // Example output:
    // output/atlas@1x-396de2aad2cb2bc6-1.webp
    // output/atlas@1x-396de2aad2cb2bc6-1.png
}
```
See [spritesheetc.h](spritesheetc/spritesheetc.h) for details, and [SpritesheetcAddon.cpp](spritesheetc-node/SpritesheetcAddon.cpp) for example usage.

## Building from source

### Prerequisites
- Git
- C++23 or later
- CMake 3.31 or later
- Rust
- Node.js

```shell
git clone https://github.com/HasangerGames/spritesheetc.git
cd spritesheetc
npm i --build-from-source
```
