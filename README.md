# spritesheetc

Ridiculously fast spritesheet generator written in C++.
Uses [resvg](https://github.com/linebender/resvg) for SVG rendering and [rectpack2D](https://github.com/TeamHypersomnia/rectpack2D) for bin packing.

`spritesheetc` functions as a command-line application, a C++ library, and a Node.js library (WIP).

## Prerequisites
- C++23 or later
- CMake 3.31 or later
- Rust

Command-line instructions below are intended for Linux. `spritesheetc` has not yet been tested on Windows or macOS.

## Building from source
```shell
git clone --recurse-submodules --shallow-submodules https://github.com/HasangerGames/spritesheetc.git
cd spritesheetc/vendored/resvg/crates/c-api
cargo build --release
cd ../../../..
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build .
```

## Installation
Assuming your working directory is the same as the previous step:
```shell
mkdir -p ~/.local/bin
cp spritesheetc ~/.local/bin
echo 'export PATH=$HOME/.local/bin:$PATH' >> ~/.bashrc # or .zshrc, etc.
```

## Usage
TBD
