# spritesheetc

Ridiculously fast spritesheet generator written in C++.
Uses [resvg](https://github.com/linebender/resvg) for SVG rendering and [rectpack2D](https://github.com/TeamHypersomnia/rectpack2D) for bin packing.

`spritesheetc` functions as a command-line application, a C++ library, and a Node.js library.
It is a high-performance, opinionated alternative to the likes of [TexturePacker](https://www.codeandweb.com/texturepacker).

## Using the Node.js API

## Using the C++ API

## Building from source

### Prerequisites
- C++23 or later
- CMake 3.31 or later
- Rust

These instructions are intended for Linux. `spritesheetc` has not yet been tested on Windows or macOS.

```shell
git clone --recurse-submodules --shallow-submodules https://github.com/HasangerGames/spritesheetc.git
cd spritesheetc/vendored/resvg/crates/c-api
cargo build --release
cd ../../../..
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build .
```

## Using the CLI
Assuming your working directory is the same as the previous step:
```shell
mkdir -p ~/.local/bin
cp spritesheetc ~/.local/bin
echo 'export PATH=$HOME/.local/bin:$PATH' >> ~/.bashrc # or .zshrc, etc.
source ~/.bashrc
```

If installation was successful, you should be able to print usage information with the following command:
```shell
spritesheetc -h
```
